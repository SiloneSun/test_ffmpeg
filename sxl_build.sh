mkdir build
cd build
rm ./* -rf
cmake ..
make -j8


strip my_test
copy_file my_test ../
copy_file my_test ~/work/tftp/