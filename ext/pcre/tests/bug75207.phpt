--TEST--
CVE-2016-1283, see bug #75207
--FILE--
<?php
preg_match("/(?:F?+(?:^(?(R)a+\"){99}-))(?J)(?'R'(?'R'<((?'RR'(?'R'\){97)?J)?J)(?'R'(?'R'\){99|(:(?|(?'R')(\k'R')|((?'R')))H'R'R)(H'R))))))/", "*b\dc");
?>
==DONE==
--EXPECTF--
Warning: preg_match(): Compilation failed: unmatched parentheses at offset %d in %s on line %d
==DONE==
