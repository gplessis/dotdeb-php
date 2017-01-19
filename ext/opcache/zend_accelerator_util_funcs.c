/*
   +----------------------------------------------------------------------+
   | Zend OPcache                                                         |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2017 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Stanislav Malyshev <stas@zend.com>                          |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

#include "zend_API.h"
#include "zend_constants.h"
#include "zend_accelerator_util_funcs.h"
#include "zend_persist.h"
#include "zend_shared_alloc.h"

#if SIZEOF_SIZE_T <= SIZEOF_ZEND_LONG
/* If sizeof(void*) == sizeof(ulong) we can use zend_hash index functions */
# define accel_xlat_set(old, new)	zend_hash_index_add_new_ptr(&ZCG(bind_hash), (zend_ulong)(zend_uintptr_t)(old), (new))
# define accel_xlat_get(old)		zend_hash_index_find_ptr(&ZCG(bind_hash), (zend_ulong)(zend_uintptr_t)(old))
#else
# define accel_xlat_set(old, new)	zend_hash_str_add_new_ptr(&ZCG(bind_hash), (char*)&(old), sizeof(void*), (old))
# define accel_xlat_get(old)	    zend_hash_str_find_ptr(&ZCG(bind_hash), (char*)&(old), sizeof(void*))
#endif

#define ARENA_REALLOC(ptr) \
	(void*)(((char*)(ptr)) + ((char*)ZCG(arena_mem) - (char*)ZCG(current_persistent_script)->arena_mem))

typedef int (*id_function_t)(void *, void *);
typedef void (*unique_copy_ctor_func_t)(void *pElement);

static zend_ast *zend_ast_clone(zend_ast *ast);

static void zend_accel_destroy_zend_function(zval *zv)
{
	zend_function *function = Z_PTR_P(zv);

	if (function->type == ZEND_USER_FUNCTION) {
		if (function->op_array.static_variables) {
			if (!(GC_FLAGS(function->op_array.static_variables) & IS_ARRAY_IMMUTABLE)) {
				if (--GC_REFCOUNT(function->op_array.static_variables) == 0) {
					FREE_HASHTABLE(function->op_array.static_variables);
				}
			}
			function->op_array.static_variables = NULL;
		}
	}

	destroy_zend_function(function);
}

static void zend_accel_destroy_zend_class(zval *zv)
{
	zend_class_entry *ce = Z_PTR_P(zv);
	ce->function_table.pDestructor = zend_accel_destroy_zend_function;
	destroy_zend_class(zv);
}

zend_persistent_script* create_persistent_script(void)
{
	zend_persistent_script *persistent_script = (zend_persistent_script *) emalloc(sizeof(zend_persistent_script));
	memset(persistent_script, 0, sizeof(zend_persistent_script));

	zend_hash_init(&persistent_script->function_table, 128, NULL, ZEND_FUNCTION_DTOR, 0);
	/* class_table is usually destroyed by free_persistent_script() that
	 * overrides destructor. ZEND_CLASS_DTOR may be used by standard
	 * PHP compiler
	 */
	zend_hash_init(&persistent_script->class_table, 16, NULL, ZEND_CLASS_DTOR, 0);

	return persistent_script;
}

void free_persistent_script(zend_persistent_script *persistent_script, int destroy_elements)
{
	if (destroy_elements) {
		persistent_script->function_table.pDestructor = zend_accel_destroy_zend_function;
		persistent_script->class_table.pDestructor = zend_accel_destroy_zend_class;
	} else {
		persistent_script->function_table.pDestructor = NULL;
		persistent_script->class_table.pDestructor = NULL;
	}

	zend_hash_destroy(&persistent_script->function_table);
	zend_hash_destroy(&persistent_script->class_table);

	if (persistent_script->full_path) {
		zend_string_release(persistent_script->full_path);
	}

	efree(persistent_script);
}

static int is_not_internal_function(zval *zv)
{
	zend_function *function = Z_PTR_P(zv);
	return(function->type != ZEND_INTERNAL_FUNCTION);
}

void zend_accel_free_user_functions(HashTable *ht)
{
	dtor_func_t orig_dtor = ht->pDestructor;

	ht->pDestructor = NULL;
	zend_hash_apply(ht, (apply_func_t) is_not_internal_function);
	ht->pDestructor = orig_dtor;
}

void zend_accel_move_user_functions(HashTable *src, HashTable *dst)
{
	Bucket *p;
	dtor_func_t orig_dtor = src->pDestructor;

	src->pDestructor = NULL;
	zend_hash_extend(dst, dst->nNumUsed + src->nNumUsed, 0);
	ZEND_HASH_REVERSE_FOREACH_BUCKET(src, p) {
		zend_function *function = Z_PTR(p->val);

		if (EXPECTED(function->type == ZEND_USER_FUNCTION)) {
			_zend_hash_append_ptr(dst, p->key, function);
			zend_hash_del_bucket(src, p);
		} else {
			break;
		}
	} ZEND_HASH_FOREACH_END();
	src->pDestructor = orig_dtor;
}

static int copy_internal_function(zval *zv, HashTable *function_table)
{
	zend_internal_function *function = Z_PTR_P(zv);
	if (function->type == ZEND_INTERNAL_FUNCTION) {
		zend_hash_update_mem(function_table, function->function_name, function, sizeof(zend_internal_function));
	}
	return 0;
}

void zend_accel_copy_internal_functions(void)
{
	zend_hash_apply_with_argument(CG(function_table), (apply_func_arg_t)copy_internal_function, &ZCG(function_table));
	ZCG(internal_functions_count) = zend_hash_num_elements(&ZCG(function_table));
}

static inline void zend_clone_zval(zval *src)
{
	void *ptr;

	if (Z_TYPE_P(src) == IS_REFERENCE) {
		ptr = accel_xlat_get(Z_REF_P(src));
		if (ptr != NULL) {
			Z_REF_P(src) = ptr;
			return;
		} else {
			zend_reference *old = Z_REF_P(src);
			ZVAL_NEW_REF(src, &old->val);
			Z_REF_P(src)->gc = old->gc;
			accel_xlat_set(old, Z_REF_P(src));
			src = Z_REFVAL_P(src);
		}
	}
	if (Z_TYPE_P(src) == IS_CONSTANT_AST) {
		if (Z_REFCOUNT_P(src) > 1 && (ptr = accel_xlat_get(Z_AST_P(src))) != NULL) {
			Z_AST_P(src) = ptr;
		} else {
			zend_ast_ref *old = Z_AST_P(src);

			ZVAL_NEW_AST(src, old->ast);
			Z_AST_P(src)->gc = old->gc;
			if (Z_REFCOUNT_P(src) > 1) {
				accel_xlat_set(old, Z_AST_P(src));
			}
			Z_ASTVAL_P(src) = zend_ast_clone(Z_ASTVAL_P(src));
		}
	}
}

static zend_ast *zend_ast_clone(zend_ast *ast)
{
	uint32_t i;

	if (ast->kind == ZEND_AST_ZVAL) {
		zend_ast_zval *copy = emalloc(sizeof(zend_ast_zval));
		copy->kind = ZEND_AST_ZVAL;
		copy->attr = ast->attr;
		ZVAL_COPY_VALUE(&copy->val, zend_ast_get_zval(ast));
		return (zend_ast *) copy;
	} else if (zend_ast_is_list(ast)) {
		zend_ast_list *list = zend_ast_get_list(ast);
		zend_ast_list *copy = emalloc(
			sizeof(zend_ast_list) - sizeof(zend_ast *) + sizeof(zend_ast *) * list->children);
		copy->kind = list->kind;
		copy->attr = list->attr;
		copy->children = list->children;
		for (i = 0; i < list->children; i++) {
			if (list->child[i]) {
				copy->child[i] = zend_ast_clone(list->child[i]);
			} else {
				copy->child[i] = NULL;
			}
		}
		return (zend_ast *) copy;
	} else {
		uint32_t children = zend_ast_get_num_children(ast);
		zend_ast *copy = emalloc(sizeof(zend_ast) - sizeof(zend_ast *) + sizeof(zend_ast *) * children);
		copy->kind = ast->kind;
		copy->attr = ast->attr;
		for (i = 0; i < children; i++) {
			if (ast->child[i]) {
				copy->child[i] = zend_ast_clone(ast->child[i]);
			} else {
				copy->child[i] = NULL;
			}
		}
		return copy;
	}
}

static void zend_hash_clone_constants(HashTable *ht, HashTable *source)
{
	Bucket *p, *q, *end;
	zend_ulong nIndex;

	ht->nTableSize = source->nTableSize;
	ht->nTableMask = source->nTableMask;
	ht->nNumUsed = 0;
	ht->nNumOfElements = source->nNumOfElements;
	ht->nNextFreeElement = source->nNextFreeElement;
	ht->pDestructor = ZVAL_PTR_DTOR;
	ht->u.flags = (source->u.flags & HASH_FLAG_INITIALIZED) | HASH_FLAG_APPLY_PROTECTION;
	ht->nInternalPointer = source->nNumOfElements ? 0 : HT_INVALID_IDX;

	if (!(ht->u.flags & HASH_FLAG_INITIALIZED)) {
		ht->arData = source->arData;
		return;
	}

	ZEND_ASSERT((source->u.flags & HASH_FLAG_PACKED) == 0);
	HT_SET_DATA_ADDR(ht, emalloc(HT_SIZE(ht)));
	HT_HASH_RESET(ht);

	p = source->arData;
	end = p + source->nNumUsed;
	for (; p != end; p++) {
		if (UNEXPECTED(Z_TYPE(p->val) == IS_UNDEF)) continue;
		nIndex = p->h | ht->nTableMask;

		/* Insert into hash collision list */
		q = ht->arData + ht->nNumUsed;
		Z_NEXT(q->val) = HT_HASH(ht, nIndex);
		HT_HASH(ht, nIndex) = HT_IDX_TO_HASH(ht->nNumUsed++);

		/* Initialize key */
		q->h = p->h;
		q->key = p->key;

		/* Copy data */
		ZVAL_COPY_VALUE(&q->val, &p->val);
		zend_clone_zval(&q->val);
	}
}

static void zend_hash_clone_methods(HashTable *ht, HashTable *source, zend_class_entry *old_ce, zend_class_entry *ce)
{
	Bucket *p, *q, *end;
	zend_ulong nIndex;
	zend_op_array *new_entry;

	ht->nTableSize = source->nTableSize;
	ht->nTableMask = source->nTableMask;
	ht->nNumUsed = 0;
	ht->nNumOfElements = source->nNumOfElements;
	ht->nNextFreeElement = source->nNextFreeElement;
	ht->pDestructor = ZEND_FUNCTION_DTOR;
	ht->u.flags = (source->u.flags & HASH_FLAG_INITIALIZED);
	ht->nInternalPointer = source->nNumOfElements ? 0 : HT_INVALID_IDX;

	if (!(ht->u.flags & HASH_FLAG_INITIALIZED)) {
		ht->arData = source->arData;
		return;
	}

	ZEND_ASSERT(!(source->u.flags & HASH_FLAG_PACKED));
	HT_SET_DATA_ADDR(ht, emalloc(HT_SIZE(ht)));
	HT_HASH_RESET(ht);

	p = source->arData;
	end = p + source->nNumUsed;
	for (; p != end; p++) {
		if (UNEXPECTED(Z_TYPE(p->val) == IS_UNDEF)) continue;

		nIndex = p->h | ht->nTableMask;

		/* Insert into hash collision list */
		q = ht->arData + ht->nNumUsed;
		Z_NEXT(q->val) = HT_HASH(ht, nIndex);
		HT_HASH(ht, nIndex) = HT_IDX_TO_HASH(ht->nNumUsed++);

		/* Initialize key */
		q->h = p->h;
		ZEND_ASSERT(p->key != NULL);
		q->key = p->key;

		/* Copy data */
		ZVAL_PTR(&q->val, ARENA_REALLOC(Z_PTR(p->val)));
		new_entry = (zend_op_array*)Z_PTR(q->val);

		if ((void*)new_entry->scope >= ZCG(current_persistent_script)->arena_mem &&
		    (void*)new_entry->scope < (void*)((char*)ZCG(current_persistent_script)->arena_mem + ZCG(current_persistent_script)->arena_size)) {

			new_entry->scope = ARENA_REALLOC(new_entry->scope);

			/* update prototype */
			if (new_entry->prototype) {
				new_entry->prototype = ARENA_REALLOC(new_entry->prototype);
			}
		}
	}
}

static void zend_hash_clone_prop_info(HashTable *ht, HashTable *source, zend_class_entry *old_ce)
{
	Bucket *p, *q, *end;
	zend_ulong nIndex;
	zend_property_info *prop_info;

	ht->nTableSize = source->nTableSize;
	ht->nTableMask = source->nTableMask;
	ht->nNumUsed = 0;
	ht->nNumOfElements = source->nNumOfElements;
	ht->nNextFreeElement = source->nNextFreeElement;
	ht->pDestructor = NULL;
	ht->u.flags = (source->u.flags & HASH_FLAG_INITIALIZED);
	ht->nInternalPointer = source->nNumOfElements ? 0 : HT_INVALID_IDX;

	if (!(ht->u.flags & HASH_FLAG_INITIALIZED)) {
		ht->arData = source->arData;
		return;
	}

	ZEND_ASSERT(!(source->u.flags & HASH_FLAG_PACKED));
	HT_SET_DATA_ADDR(ht, emalloc(HT_SIZE(ht)));
	HT_HASH_RESET(ht);

	p = source->arData;
	end = p + source->nNumUsed;
	for (; p != end; p++) {
		if (UNEXPECTED(Z_TYPE(p->val) == IS_UNDEF)) continue;

		nIndex = p->h | ht->nTableMask;

		/* Insert into hash collision list */
		q = ht->arData + ht->nNumUsed;
		Z_NEXT(q->val) = HT_HASH(ht, nIndex);
		HT_HASH(ht, nIndex) = HT_IDX_TO_HASH(ht->nNumUsed++);

		/* Initialize key */
		q->h = p->h;
		ZEND_ASSERT(p->key != NULL);
		q->key = p->key;

		/* Copy data */
		prop_info = ARENA_REALLOC(Z_PTR(p->val));
		ZVAL_PTR(&q->val, prop_info);

		if ((void*)prop_info->ce >= ZCG(current_persistent_script)->arena_mem &&
		    (void*)prop_info->ce < (void*)((char*)ZCG(current_persistent_script)->arena_mem + ZCG(current_persistent_script)->arena_size)) {
			prop_info->ce = ARENA_REALLOC(prop_info->ce);
		}
	}
}

#define zend_update_inherited_handler(handler) \
{ \
	if (ce->handler != NULL) { \
		ce->handler = ARENA_REALLOC(ce->handler); \
	} \
}

/* Protects class' refcount, copies default properties, functions and class name */
static void zend_class_copy_ctor(zend_class_entry **pce)
{
	zend_class_entry *ce = *pce;
	zend_class_entry *old_ce = ce;
	zval *src, *dst, *end;

	*pce = ce = ARENA_REALLOC(old_ce);
	ce->refcount = 1;

	if (old_ce->default_properties_table) {
		ce->default_properties_table = emalloc(sizeof(zval) * old_ce->default_properties_count);
		src = old_ce->default_properties_table;
		end = src + old_ce->default_properties_count;
		dst = ce->default_properties_table;
		for (; src != end; src++, dst++) {
			ZVAL_COPY_VALUE(dst, src);
			zend_clone_zval(dst);
		}
	}

	zend_hash_clone_methods(&ce->function_table, &old_ce->function_table, old_ce, ce);

	/* static members */
	if (old_ce->default_static_members_table) {
		ce->default_static_members_table = emalloc(sizeof(zval) * old_ce->default_static_members_count);
		src = old_ce->default_static_members_table;
		end = src + old_ce->default_static_members_count;
		dst = ce->default_static_members_table;
		for (; src != end; src++, dst++) {
			ZVAL_COPY_VALUE(dst, src);
			zend_clone_zval(dst);
		}
	}
	ce->static_members_table = ce->default_static_members_table;

	/* properties_info */
	zend_hash_clone_prop_info(&ce->properties_info, &old_ce->properties_info, old_ce);

	/* constants table */
	zend_hash_clone_constants(&ce->constants_table, &old_ce->constants_table);
	ce->constants_table.u.flags &= ~HASH_FLAG_APPLY_PROTECTION;

	/* interfaces aren't really implemented, so we create a new table */
	if (ce->num_interfaces) {
		ce->interfaces = emalloc(sizeof(zend_class_entry *) * ce->num_interfaces);
		memset(ce->interfaces, 0, sizeof(zend_class_entry *) * ce->num_interfaces);
	} else {
		ce->interfaces = NULL;
	}

	if (ce->parent) {
		ce->parent = ARENA_REALLOC(ce->parent);
	}

	zend_update_inherited_handler(constructor);
	zend_update_inherited_handler(destructor);
	zend_update_inherited_handler(clone);
	zend_update_inherited_handler(__get);
	zend_update_inherited_handler(__set);
	zend_update_inherited_handler(__call);
/* 5.1 stuff */
	zend_update_inherited_handler(serialize_func);
	zend_update_inherited_handler(unserialize_func);
	zend_update_inherited_handler(__isset);
	zend_update_inherited_handler(__unset);
/* 5.2 stuff */
	zend_update_inherited_handler(__tostring);

/* 5.3 stuff */
	zend_update_inherited_handler(__callstatic);
	zend_update_inherited_handler(__debugInfo);

/* 5.4 traits */
	if (ce->trait_aliases) {
		zend_trait_alias **trait_aliases;
		int i = 0;

		while (ce->trait_aliases[i]) {
			i++;
		}
		trait_aliases = emalloc(sizeof(zend_trait_alias*) * (i + 1));
		i = 0;
		while (ce->trait_aliases[i]) {
			trait_aliases[i] = emalloc(sizeof(zend_trait_alias));
			memcpy(trait_aliases[i], ce->trait_aliases[i], sizeof(zend_trait_alias));
			trait_aliases[i]->trait_method = emalloc(sizeof(zend_trait_method_reference));
			memcpy(trait_aliases[i]->trait_method, ce->trait_aliases[i]->trait_method, sizeof(zend_trait_method_reference));
			i++;
		}
		trait_aliases[i] = NULL;
		ce->trait_aliases = trait_aliases;
	}

	if (ce->trait_precedences) {
		zend_trait_precedence **trait_precedences;
		int i = 0;

		while (ce->trait_precedences[i]) {
			i++;
		}
		trait_precedences = emalloc(sizeof(zend_trait_precedence*) * (i + 1));
		i = 0;
		while (ce->trait_precedences[i]) {
			trait_precedences[i] = emalloc(sizeof(zend_trait_precedence));
			memcpy(trait_precedences[i], ce->trait_precedences[i], sizeof(zend_trait_precedence));
			trait_precedences[i]->trait_method = emalloc(sizeof(zend_trait_method_reference));
			memcpy(trait_precedences[i]->trait_method, ce->trait_precedences[i]->trait_method, sizeof(zend_trait_method_reference));

			if (trait_precedences[i]->exclude_from_classes) {
				zend_string **exclude_from_classes;
				int j = 0;

				while (trait_precedences[i]->exclude_from_classes[j].class_name) {
					j++;
				}
				exclude_from_classes = emalloc(sizeof(zend_string*) * (j + 1));
				j = 0;
				while (trait_precedences[i]->exclude_from_classes[j].class_name) {
					exclude_from_classes[j] =
						trait_precedences[i]->exclude_from_classes[j].class_name;
					j++;
				}
				exclude_from_classes[j] = NULL;
				trait_precedences[i]->exclude_from_classes = (void*)exclude_from_classes;
			}
			i++;
		}
		trait_precedences[i] = NULL;
		ce->trait_precedences = trait_precedences;
	}
}

static void zend_accel_function_hash_copy(HashTable *target, HashTable *source)
{
	zend_function *function1, *function2;
	Bucket *p, *end;
	zval *t;

	zend_hash_extend(target, target->nNumUsed + source->nNumUsed, 0);
	p = source->arData;
	end = p + source->nNumUsed;
	for (; p != end; p++) {
		if (UNEXPECTED(Z_TYPE(p->val) == IS_UNDEF)) continue;
		ZEND_ASSERT(p->key);
		t = zend_hash_find(target, p->key);
		if (UNEXPECTED(t != NULL)) {
			if (EXPECTED(ZSTR_LEN(p->key) > 0) && EXPECTED(ZSTR_VAL(p->key)[0] == 0)) {
				/* Mangled key */
				t = zend_hash_update(target, p->key, &p->val);
			} else {
				goto failure;
			}
		} else {
			_zend_hash_append_ptr(target, p->key, Z_PTR(p->val));
		}
	}
	target->nInternalPointer = target->nNumOfElements ? 0 : HT_INVALID_IDX;
	return;

failure:
	function1 = Z_PTR(p->val);
	function2 = Z_PTR_P(t);
	CG(in_compilation) = 1;
	zend_set_compiled_filename(function1->op_array.filename);
	CG(zend_lineno) = function1->op_array.opcodes[0].lineno;
	if (function2->type == ZEND_USER_FUNCTION
		&& function2->op_array.last > 0) {
		zend_error(E_ERROR, "Cannot redeclare %s() (previously declared in %s:%d)",
				   ZSTR_VAL(function1->common.function_name),
				   ZSTR_VAL(function2->op_array.filename),
				   (int)function2->op_array.opcodes[0].lineno);
	} else {
		zend_error(E_ERROR, "Cannot redeclare %s()", ZSTR_VAL(function1->common.function_name));
	}
}

static void zend_accel_function_hash_copy_from_shm(HashTable *target, HashTable *source)
{
	zend_function *function1, *function2;
	Bucket *p, *end;
	zval *t;

	zend_hash_extend(target, target->nNumUsed + source->nNumUsed, 0);
	p = source->arData;
	end = p + source->nNumUsed;
	for (; p != end; p++) {
		if (UNEXPECTED(Z_TYPE(p->val) == IS_UNDEF)) continue;
		ZEND_ASSERT(p->key);
		t = zend_hash_find(target, p->key);
		if (UNEXPECTED(t != NULL)) {
			if (EXPECTED(ZSTR_LEN(p->key) > 0) && EXPECTED(ZSTR_VAL(p->key)[0] == 0)) {
				/* Mangled key */
				zend_hash_update_ptr(target, p->key, ARENA_REALLOC(Z_PTR(p->val)));
			} else {
				goto failure;
			}
		} else {
			_zend_hash_append_ptr(target, p->key, ARENA_REALLOC(Z_PTR(p->val)));
		}
	}
	target->nInternalPointer = target->nNumOfElements ? 0 : HT_INVALID_IDX;
	return;

failure:
	function1 = Z_PTR(p->val);
	function2 = Z_PTR_P(t);
	CG(in_compilation) = 1;
	zend_set_compiled_filename(function1->op_array.filename);
	CG(zend_lineno) = function1->op_array.opcodes[0].lineno;
	if (function2->type == ZEND_USER_FUNCTION
		&& function2->op_array.last > 0) {
		zend_error(E_ERROR, "Cannot redeclare %s() (previously declared in %s:%d)",
				   ZSTR_VAL(function1->common.function_name),
				   ZSTR_VAL(function2->op_array.filename),
				   (int)function2->op_array.opcodes[0].lineno);
	} else {
		zend_error(E_ERROR, "Cannot redeclare %s()", ZSTR_VAL(function1->common.function_name));
	}
}

static void zend_accel_class_hash_copy(HashTable *target, HashTable *source, unique_copy_ctor_func_t pCopyConstructor)
{
	Bucket *p, *end;
	zval *t;

	zend_hash_extend(target, target->nNumUsed + source->nNumUsed, 0);
	p = source->arData;
	end = p + source->nNumUsed;
	for (; p != end; p++) {
		if (UNEXPECTED(Z_TYPE(p->val) == IS_UNDEF)) continue;
		ZEND_ASSERT(p->key);
		t = zend_hash_find(target, p->key);
		if (UNEXPECTED(t != NULL)) {
			if (EXPECTED(ZSTR_LEN(p->key) > 0) && EXPECTED(ZSTR_VAL(p->key)[0] == 0)) {
				/* Mangled key - ignore and wait for runtime */
				continue;
			} else if (UNEXPECTED(!ZCG(accel_directives).ignore_dups)) {
				zend_class_entry *ce1 = Z_PTR(p->val);
				if (!(ce1->ce_flags & ZEND_ACC_ANON_CLASS)) {
					CG(in_compilation) = 1;
					zend_set_compiled_filename(ce1->info.user.filename);
					CG(zend_lineno) = ce1->info.user.line_start;
					zend_error(E_ERROR,
							"Cannot declare %s %s, because the name is already in use",
							zend_get_object_type(ce1), ZSTR_VAL(ce1->name));
					return;
				}
				continue;
			}
		} else {
			t = _zend_hash_append_ptr(target, p->key, Z_PTR(p->val));
			if (pCopyConstructor) {
				pCopyConstructor(&Z_PTR_P(t));
			}
		}
	}
	target->nInternalPointer = target->nNumOfElements ? 0 : HT_INVALID_IDX;
	return;
}

#ifdef __SSE2__
#include <mmintrin.h>
#include <emmintrin.h>

static zend_always_inline void fast_memcpy(void *dest, const void *src, size_t size)
{
	__m128i *dqdest = (__m128i*)dest;
	const __m128i *dqsrc  = (const __m128i*)src;
	const __m128i *end  = (const __m128i*)((const char*)src + size);

	do {
		_mm_prefetch(dqsrc + 4, _MM_HINT_NTA);
		_mm_prefetch(dqdest + 4, _MM_HINT_T0);

		__m128i xmm0 = _mm_load_si128(dqsrc + 0);
		__m128i xmm1 = _mm_load_si128(dqsrc + 1);
		__m128i xmm2 = _mm_load_si128(dqsrc + 2);
		__m128i xmm3 = _mm_load_si128(dqsrc + 3);
		dqsrc  += 4;
		_mm_store_si128(dqdest + 0, xmm0);
		_mm_store_si128(dqdest + 1, xmm1);
		_mm_store_si128(dqdest + 2, xmm2);
		_mm_store_si128(dqdest + 3, xmm3);
		dqdest += 4;
	} while (dqsrc != end);
}
#endif

zend_op_array* zend_accel_load_script(zend_persistent_script *persistent_script, int from_shared_memory)
{
	zend_op_array *op_array;

	op_array = (zend_op_array *) emalloc(sizeof(zend_op_array));
	*op_array = persistent_script->main_op_array;

	if (EXPECTED(from_shared_memory)) {
		zend_hash_init(&ZCG(bind_hash), 10, NULL, NULL, 0);

		ZCG(current_persistent_script) = persistent_script;
		ZCG(arena_mem) = NULL;
		if (EXPECTED(persistent_script->arena_size)) {
#ifdef __SSE2__
			/* Target address must be aligned to 64-byte boundary */
			ZCG(arena_mem) = zend_arena_alloc(&CG(arena), persistent_script->arena_size + 64);
			ZCG(arena_mem) = (void*)(((zend_uintptr_t)ZCG(arena_mem) + 63L) & ~63L);
			fast_memcpy(ZCG(arena_mem), persistent_script->arena_mem, persistent_script->arena_size);
#else
			ZCG(arena_mem) = zend_arena_alloc(&CG(arena), persistent_script->arena_size);
			memcpy(ZCG(arena_mem), persistent_script->arena_mem, persistent_script->arena_size);
#endif
		}

		/* Copy all the necessary stuff from shared memory to regular memory, and protect the shared script */
		if (zend_hash_num_elements(&persistent_script->class_table) > 0) {
			zend_accel_class_hash_copy(CG(class_table), &persistent_script->class_table, (unique_copy_ctor_func_t) zend_class_copy_ctor);
		}
		/* we must first to copy all classes and then prepare functions, since functions may try to bind
		   classes - which depend on pre-bind class entries existent in the class table */
		if (zend_hash_num_elements(&persistent_script->function_table) > 0) {
			zend_accel_function_hash_copy_from_shm(CG(function_table), &persistent_script->function_table);
		}

		/* Register __COMPILER_HALT_OFFSET__ constant */
		if (persistent_script->compiler_halt_offset != 0 &&
		    persistent_script->full_path) {
			zend_string *name;
			char haltoff[] = "__COMPILER_HALT_OFFSET__";

			name = zend_mangle_property_name(haltoff, sizeof(haltoff) - 1, ZSTR_VAL(persistent_script->full_path), ZSTR_LEN(persistent_script->full_path), 0);
			if (!zend_hash_exists(EG(zend_constants), name)) {
				zend_register_long_constant(ZSTR_VAL(name), ZSTR_LEN(name), persistent_script->compiler_halt_offset, CONST_CS, 0);
			}
			zend_string_release(name);
		}

		zend_hash_destroy(&ZCG(bind_hash));
		ZCG(current_persistent_script) = NULL;
	} else /* if (!from_shared_memory) */ {
		if (zend_hash_num_elements(&persistent_script->function_table) > 0) {
			zend_accel_function_hash_copy(CG(function_table), &persistent_script->function_table);
		}
		if (zend_hash_num_elements(&persistent_script->class_table) > 0) {
			zend_accel_class_hash_copy(CG(class_table), &persistent_script->class_table, NULL);
		}
	}

	if (op_array->early_binding != (uint32_t)-1) {
		zend_string *orig_compiled_filename = CG(compiled_filename);
		CG(compiled_filename) = persistent_script->full_path;
		zend_do_delayed_early_binding(op_array);
		CG(compiled_filename) = orig_compiled_filename;
	}

	if (UNEXPECTED(!from_shared_memory)) {
		free_persistent_script(persistent_script, 0); /* free only hashes */
	}

	return op_array;
}

/*
 * zend_adler32() is based on zlib implementation
 * Computes the Adler-32 checksum of a data stream
 *
 * Copyright (C) 1995-2005 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Copyright (C) 1995-2005 Jean-loup Gailly and Mark Adler
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty.  In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 *
 */

#define ADLER32_BASE 65521 /* largest prime smaller than 65536 */
#define ADLER32_NMAX 5552
/* NMAX is the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1 */

#define ADLER32_DO1(buf)        {s1 += *(buf); s2 += s1;}
#define ADLER32_DO2(buf, i)     ADLER32_DO1(buf + i); ADLER32_DO1(buf + i + 1);
#define ADLER32_DO4(buf, i)     ADLER32_DO2(buf, i); ADLER32_DO2(buf, i + 2);
#define ADLER32_DO8(buf, i)     ADLER32_DO4(buf, i); ADLER32_DO4(buf, i + 4);
#define ADLER32_DO16(buf)       ADLER32_DO8(buf, 0); ADLER32_DO8(buf, 8);

unsigned int zend_adler32(unsigned int checksum, signed char *buf, uint len)
{
	unsigned int s1 = checksum & 0xffff;
	unsigned int s2 = (checksum >> 16) & 0xffff;
	signed char *end;

	while (len >= ADLER32_NMAX) {
		len -= ADLER32_NMAX;
		end = buf + ADLER32_NMAX;
		do {
			ADLER32_DO16(buf);
			buf += 16;
		} while (buf != end);
		s1 %= ADLER32_BASE;
		s2 %= ADLER32_BASE;
	}

	if (len) {
		if (len >= 16) {
			end = buf + (len & 0xfff0);
			len &= 0xf;
			do {
				ADLER32_DO16(buf);
				buf += 16;
			} while (buf != end);
		}
		if (len) {
			end = buf + len;
			do {
				ADLER32_DO1(buf);
				buf++;
			} while (buf != end);
		}
		s1 %= ADLER32_BASE;
		s2 %= ADLER32_BASE;
	}

	return (s2 << 16) | s1;
}

unsigned int zend_accel_script_checksum(zend_persistent_script *persistent_script)
{
	signed char *mem = (signed char*)persistent_script->mem;
	size_t size = persistent_script->size;
	size_t persistent_script_check_block_size = ((char *)&(persistent_script->dynamic_members)) - (char *)persistent_script;
	unsigned int checksum = ADLER32_INIT;

	if (mem < (signed char*)persistent_script) {
		checksum = zend_adler32(checksum, mem, (signed char*)persistent_script - mem);
		size -= (signed char*)persistent_script - mem;
		mem  += (signed char*)persistent_script - mem;
	}

	zend_adler32(checksum, mem, persistent_script_check_block_size);
	mem  += sizeof(*persistent_script);
	size -= sizeof(*persistent_script);

	if (size > 0) {
		checksum = zend_adler32(checksum, mem, size);
	}
	return checksum;
}
