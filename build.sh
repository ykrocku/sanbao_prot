
mkdir -p arm
cd arm
if [ "$1" == "a" ] ; then
../adas_cmake.sh
else
../dsm_cmake.sh
fi
make
