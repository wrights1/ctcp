USERNAME=`whoami`
HOMEDIR=/home/$USERNAME

if [ "$#" -ne 2 ]; then
    echo "Illegal number of parameters"
fi

TARNAME=$1
LABNAME=$2

PRINTF=printf

$PRINTF "Creating your submission tar...\n"
tar -zcvf $TARNAME *.c *.h lab12_README Makefile

$PRINTF "Created a tarball of your assignment called $TARNAME.\n"
