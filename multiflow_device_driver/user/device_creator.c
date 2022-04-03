#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

int i;
char buff[4096];


int main(int argc, char** argv){

    int ret;
    int major;
    int minors;
    char *path;

    if(argc<4){
        printf("useg: prog pathname major minors\n");
        return -1;
    }

    path = argv[1];
    major = strtol(argv[2], NULL, 10);
    minors = strtol(argv[3], NULL, 10);
    printf("creating %d minors for device %s with major %d\n", minors, path, major);

    for(i = 0; i < minors; i++){
	    sprintf(buff, "mknod %s%d c %d %i\n", path, i, major, i);
	    system(buff);
	    sprintf(buff, "%s%d", path, i);
    }
    return 0;

}