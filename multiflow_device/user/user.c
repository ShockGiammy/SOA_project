#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

int i;
char buff[4096];
#define DATA "ciao a tutti\n"
#define SIZE strlen(DATA)  //25

int open_device(char* device);
//int create_device(char* path, long major, long minor);

void * the_thread(){

	char* device;
	int fd;
	int decision;
	char* cmd;
	//long major = (long)info;
	//int i = 0;

	while(1) {
		memset(cmd, 0, sizeof(char)*(strlen(cmd)+1));
		memset(device, 0, sizeof(char)*(strlen(device)+1));
		printf("Choose an operation to perform on the device driver:\n");
		//printf("1) Create a new device driver\n");
		printf("1) Open an existing device driver\n");
		printf("2) Write data from opened device\n");
		printf("3) Read data from opened device\n");
		printf("4) Call the ioctl() service\n");
		printf("5) Close the device\n");
		fgets(cmd, 5, stdin);
		decision = strtol(cmd, NULL, 10);
		switch(decision) {
			/*case 1:
				printf("What device driver do you want to create?\n");
				scanf("%s", device);
				create_device(device, major, i);
				i++;
				close(fd);
				fd = open_device(device);
				break;*/
			case 1:
				printf("What device driver do you want to open?\n");
				fgets(device, 4096, stdin);
				device[strcspn(device, "\n")] = 0;
				//close(fd);
				fd = open_device(device);
				break;
			case 2:
				write(fd, DATA, SIZE);
				break;
			case 3:
				read(fd, buff, SIZE);
				printf("%s\n", buff);
				break;
			case 4:
				ioctl(fd,0);
				break;
			case 5:
				printf("Bye bye!");
				close(fd);
				exit(0);
			default:
				printf("Operation not permitted\n");
				break;
		}
	}
	return NULL;
}


/*int create_device(char* path, long major, long minor) {

    printf("creating minor number %ld for device %s with major %ld\n", minor, path, major);

    sprintf(buff, "mknod %s%d c %ld %i\n", path, i, major, i);
	system(buff);
	sprintf(buff, "%s%d", path, i);
	return 0;
}*/


int open_device(char* device) {
	int fd;

	printf("opening device %s\n", device);
	fd = open(device, O_RDWR);
	if (fd == -1) {
		printf("open error on device %s\n", device);
		return -1;
	}
	printf("device %s successfully opened\n", device);
	return fd;
}


int main(int argc, char** argv){

    pthread_t tid;

	pthread_create(&tid, NULL, the_thread, NULL);

    pause();
    return 0;
}
