#include <iostream>


#include <string>
#include "t_log.h"
#include "md_file.h"

using namespace sunxilong;

int main(int argc, char* argv[])
{
    const char* filePath = (argc > 1) ? argv[1] : "./res/Video_19700101_003613642_thumb.mp4";
    std::shared_ptr<md_file> mdfile = md_file::get(std::string(filePath));
    mdfile->open_file();
    mdfile->play();
    
    
    LOGD("test end");
    mdfile->close_file();

    mdfile.reset();

    
    return 0;
}