#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

//int i;

int open_device(char* device); 
//int create_device(char* path, long major, long minor);


/*int create_device(char* path, long major, long minor) {

    printf("creating minor number %ld for device %s with major %ld\n", minor, path, major);

    sprintf(buff, "mknod %s%d c %ld %i\n", path, i, major, i);
	system(buff);
	sprintf(buff, "%s%d", path, i);
	return 0;
}*/


int open_device(char* device) {

	int fd;

	printf("\nopening device %s\n", device);
	fd = open(device, O_RDWR);
	if (fd == -1) {
		printf("open error on device %s\n", device);
		return -1;
	}
	printf("device %s successfully opened\n", device);
	return fd;
}


int call_ioctl(int fd) {

	char cmd[3];
	int ret;
	int decision;
	char time[50];

	printf("\nSelect the configuration to modify:\n");
	printf("1) Change the priority level to HIGH\n");
	printf("2) Change the priority level to LOW\n");
	printf("3) Set the BLOCKING operation mode\n");
	printf("4) Set the NON-BLOCKING operation mode\n");
	printf("5) DISABLE the device\n");
	printf("6) ENABLE the device\n");
	fgets(cmd, 3, stdin);
	decision = strtol(cmd, NULL, 10);
	switch(decision) {
		case 1:
			ret = ioctl(fd, 0);
			if (ret == -1) {
				printf("An error is occured\n");
				return -1;
			}
			printf("Priority level set to HIGH\n");
			break;
		case 2:
			ret = ioctl(fd, 1);
			if (ret == -1) {
				printf("An error is occured\n");
				return -1;
			}
			printf("Priority level set to LOW\n");
			break;
		case 3:
			printf("You need to choose the wake up timeout\n	Insert a valid time (millisecond):");
			fgets(time, 50, stdin);
			unsigned long timeout = strtol(time, NULL, 10);
			while(timeout == 0) {
				memset(time, 0, sizeof(char)*(strlen(time)+1));
				printf("Please insert a valid time:\n");
				fgets(time, 50, stdin);
				unsigned long timeout = strtol(time, NULL, 10);
			}
			ret = ioctl(fd, 3);
			if (ret == -1) {
				printf("An error is occured in setting BLOCKING mode\n");
				return -1;
			}
			ret = ioctl(fd, 5, timeout);
			if (ret == -1) {
				printf("An error is occured in setting the timeout\n");
				return -1;
			}
			printf("Operations are BLOCKING and timeout is set to the value %ld ms\n", timeout);
			break;
		case 4:
			ret = ioctl(fd, 4);
			if (ret == -1) {
				printf("An error is occured");
				return -1;
			}
			printf("Operations are NON-BLOCKING\n");
			break;
		case 5:
			ret = ioctl(fd, 7);
			if (ret == -1) {
				printf("An error is occured\n");
				return -1;
			}
			printf("Device is disabled. It's not possible to open new sessions to the device\n");
			printf("Open sessions will be still managed\n");
			break;
		case 6:
			ret = ioctl(fd, 6);
			if (ret == -1) {
				printf("An error is occured\n");
				return -1;
			}
			printf("Device is ready to receive new sessions again\n");
			break;
		default:
			printf("Unknown option\n");
			break;
	}
	return 0;
}


int main(int argc, char** argv){

    char buff[4096];
	char data[4096];
	char device[50];
	int fd = -1;
	int decision;
	char cmd[3];
	int ret;
	//long major = (long)info;
	//int i = 0;

	while(1) {
		memset(cmd, 0, sizeof(char)*(strlen(cmd)+1));
		printf("\nChoose an operation to perform on the device driver:\n");
		//printf("0) Create a new device driver\n");
		printf("1) Open an existing device driver\n");
		printf("2) Write data from opened device\n");
		printf("3) Read data from opened device\n");
		printf("4) Call the ioctl() service\n");
		printf("5) Close the device\n");
		fgets(cmd, 3, stdin);
		decision = strtol(cmd, NULL, 10);
		switch(decision) {
			/*case 0:
				printf("What device driver do you want to create?\n");
				scanf("%s", device);
				create_device(device, major, i);
				i++;
				close(fd);
				fd = open_device(device);
				break;*/
			case 1:
				if (fd != -1) {
					memset(device, 0, sizeof(char)*(strlen(device)+1));
					close(fd);
				}
				printf("\nWhat device driver do you want to open?\n");
				fgets(device, 50, stdin);
				device[strcspn(device, "\n")] = 0;
				fd = open_device(device);
				break;
			case 2:
				if (fd == -1) {
					printf("Please, is first required opening a suitable device driver\n");
				}
				else {
					printf("\nWhat data do you want to write?\n");
					fgets(data, 4096, stdin);
					ret = write(fd, data, strlen(data));
					if (ret == -1) {
						printf("Error in write operation\n");
					}
					else {
						printf("%d bytes are correctly written to device %s\n", ret, device);
					}
				}
				memset(data, 0, sizeof(char)*(strlen(data)+1));
				break;
			case 3:
				if (fd == -1) {
					printf("Please, is first required opening a suitable device driver\n");
				}
				else {
					printf("\nHow many data do you want to read?\n");
					fgets(data, 4096, stdin);
					int len = strtol(data, NULL, 10);
					ret = read(fd, buff, len);
					if (ret == -1) {
						printf("Not enough data to read\n");
					}
					else {
						printf("%d bytes are correctly read from device %s\n", ret, device);
					}
					printf("%s", buff);
					memset(buff, 0, sizeof(char)*(strlen(buff)+1));
				}
				memset(data, 0, sizeof(char)*(strlen(data)+1));
				break;
			case 4:
				if (fd == -1) {
					printf("Please, is first required opening a suitable device driver\n");
				}
				else {
					call_ioctl(fd);
				}
				break;
			case 5:
				printf("\nBye bye!\n");
				if (fd != -1) {
					ret = close(fd);
					if (ret == 0) {
						printf("Device %s is correctly closed\n", device);
					}
					else {
						printf("Error in close\n");
					}
				}
				exit(0);
			default:
				printf("\nOperation not permitted\n");
				break;
		}
	}
	return 0;
}
