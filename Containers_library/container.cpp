#include <iostream>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#include <cstring>
#include <dirent.h>

#define STACK_SIZE 8192
#define ERROR_MSG "system error: %s\n"
#define ARGC_MAX_DIGITS 5
#define CGROUP_DIR_PERMISSIONS 0755
#define SUCCESS 0
#define FAILURE -1


// ============================= FUNCTION DECLARATIONS ========================
int entryPoint(void* args);
void destroyContainer (char* fileSystemPath);
int recursivelyDeleteDirectory(const char *path);

// ============================= HELPER FUNCTIONS =============================
int entryPoint(void* args)
{
    char** argumentsToParse = (char**)args;
    int argc = (int)(strtol(argumentsToParse[0], nullptr, 10));
    char* newHostname = argumentsToParse[1];
    char* newFileSystemPath = argumentsToParse[2];
    int processesAmount = (int)(strtol(argumentsToParse[3], nullptr, 10));
    char* pathToProgram = argumentsToParse[4];

    char* argumentsForProgram[(argc - 5) + 2]; // argc-5 is the num of arguments and the -2 is for the program name+NULL at the end
    argumentsForProgram[0] = pathToProgram;
    for(int i = 1; i <= argc - 5; ++i){
        argumentsForProgram[i] = argumentsToParse[i + 4];
    }
    argumentsForProgram[(argc - 5) + 1] = nullptr;

    //stage 2:
    //2.1 - changing host name
    if(sethostname(newHostname, strlen(newHostname)) != SUCCESS)
    {
        fprintf(stderr, ERROR_MSG, "set new host name failed");
        exit(1);
    }

    //2.1 - changing FS root
    if(chroot(newFileSystemPath) != SUCCESS)
    {
        fprintf(stderr, ERROR_MSG, "changing root directory to fs failed");
        exit(1);
    }

    //2.2 - creating cGroups directory
    if(mkdir("/sys/fs", CGROUP_DIR_PERMISSIONS) != SUCCESS){
        fprintf(stderr, ERROR_MSG, "creating /sys/fs directory failed");
        exit(1);
    }

    if(mkdir("/sys/fs/cgroup", CGROUP_DIR_PERMISSIONS) != SUCCESS){
        fprintf(stderr, ERROR_MSG, "creating /sys/fs/cgroup directory failed");
        exit(1);
    }

    if(mkdir("/sys/fs/cgroup/pids", CGROUP_DIR_PERMISSIONS) != SUCCESS){
        fprintf(stderr, ERROR_MSG, "creating /sys/fs/cgroup/pids directory failed");
        exit(1);
    }


    //2.2 - cgroup files
    FILE* cgroup_procs = fopen("/sys/fs/cgroup/pids/cgroup.procs", "w+");
    if(cgroup_procs == nullptr){
        fprintf(stderr, ERROR_MSG, "creating cgroup.procs failed");
        exit(1);
    }
    chmod("/sys/fs/cgroup/pids/cgroup.procs", CGROUP_DIR_PERMISSIONS);
    fprintf(cgroup_procs, "%d", (int)getpid());
    fclose(cgroup_procs);

    FILE* pids_max = fopen("/sys/fs/cgroup/pids/pids.max", "w+");
    if(pids_max == nullptr){
        fprintf(stderr, ERROR_MSG, "creating pids.max failed");
        exit(1);
    }
    chmod("/sys/fs/cgroup/pids/pids.max", CGROUP_DIR_PERMISSIONS);
    fprintf(pids_max, "%d", processesAmount);
    fclose(pids_max);

    FILE* notify_on_release = fopen("/sys/fs/cgroup/pids/notify_on_release", "w+");
    if(notify_on_release == nullptr){
        fprintf(stderr, ERROR_MSG, "creating notify_on_release failed");
        exit(1);
    }
    chmod("/sys/fs/cgroup/pids/notify_on_release", CGROUP_DIR_PERMISSIONS);
    fprintf(notify_on_release, "%d", 1);
    fclose(notify_on_release);


    //2.3 - changing working directory
    if(chdir(newFileSystemPath) != SUCCESS){
        fprintf(stderr, ERROR_MSG, "changing working directory failed");
        exit(1);
    }

    //2.4 - mount procs
    if(mount("proc", "/proc", "proc", 0, 0) != SUCCESS){
        fprintf(stderr, ERROR_MSG, "mount proc failed");
        exit(1);
    }

//    //2.5 - run the program
    if(execvp(pathToProgram, argumentsForProgram) == FAILURE)
    {
        fprintf(stderr, ERROR_MSG, "execvp command executing failed");
        fprintf(stderr, ERROR_MSG, strerror(errno));
        exit(1);
    }
    return 0;
}


int recursivelyDeleteDirectory(const char *path) {
    DIR *dir = opendir(path);
    size_t pathLength = strlen(path);
    int r = -1;

    if (dir) {
        struct dirent *direntPointer;

        r = 0;
        while (!r && (direntPointer = readdir(dir))) {
            int r2 = -1;
            char *buf;
            size_t len;

            if (!strcmp(direntPointer->d_name, ".") || !strcmp(direntPointer->d_name, ".."))
                continue;

            len = pathLength + strlen(direntPointer->d_name) + 2;
            buf = (char*)malloc(len);

            if (buf) {
                struct stat statbuf;

                snprintf(buf, len, "%s/%s", path, direntPointer->d_name);
                if (!stat(buf, &statbuf)) {
                    if (S_ISDIR(statbuf.st_mode))
                        r2 = recursivelyDeleteDirectory (buf);
                    else
                        r2 = unlink(buf);
                }
                free(buf);
            }
            r = r2;
        }
        closedir(dir);
    }

    if (!r)
        r = rmdir(path);

    return r;
}


void destroyContainer (char* fileSystemPath)
{
    //stage 3 - unmount file system
    char* procPath = (char *) calloc(strlen(fileSystemPath) + 10, sizeof(char));
    procPath = strcat(procPath, fileSystemPath);
    procPath = strcat(procPath, "/proc");
    umount(procPath);
    free(procPath);

    //stage 4 - delete created dirs
    char* fsPath = (char *) calloc(strlen(fileSystemPath) + 30, sizeof(char));
    fsPath =  strcat(fsPath, fileSystemPath);
    fsPath =  strcat(fsPath, "/sys/fs");
    recursivelyDeleteDirectory (fsPath);
    free(fsPath);
}

// ============================= MAIN PROGRAM =============================
//COMMAND TO RUN FROM SHELL:
// /container <hostname> <new filesystem dir> <#processes> <path to program> <args for program>

int main (int argc, char* argv[])
{
    if(argc < 5){
        fprintf(stderr, ERROR_MSG, "Insufficient Number Of Arguments");
        exit(1);
    }

    char buffer[ARGC_MAX_DIGITS];
    if(sprintf(buffer, "%d", argc) < 0){
        fprintf(stderr, ERROR_MSG, "argc Integer Conversion Failed");
        exit(1);
    }
    argv[0] = buffer;

    //stage 1 - allocating stack
    char* stack = (char*)malloc(STACK_SIZE);
    if (stack == nullptr)
    {
        fprintf(stderr, ERROR_MSG, "Memory Allocation FAILED");
        exit(1);
    }

    clone(entryPoint, stack + STACK_SIZE, (SIGCHLD | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS), argv);
    wait(nullptr);

    destroyContainer(argv[2]);
    free(stack);
}