mkdir build
cd build
rm ./* -rf
cmake ..
make -j8

copy_file my_test ../
copy_file my_test ~/work/tftp/