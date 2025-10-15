#!/bin/bash

TESTSDIR=test
TESTS=$(ls $TESTSDIR)

for t in $TESTS; do
	echo =========[[ RUNNING TEST $t ]]============
	bash ./$TESTSDIR/$t && 
		echo +++++++++[[ TEST $t SUCCESSFUL! ]]++++++++++ ||
		{
			echo !!!!!!!!!!![[ TEST $t *FAILED* ]]!!!!!!!!!!!!!
			exit 1
		}
	echo ""
done

