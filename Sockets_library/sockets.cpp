#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <armadillo>

#define ERROR_MSG "system error: %s\n"
#define MAX_HOST_NAME_LEN 256
#define MAX_CLIENTS_TO_LISTEN 5
#define MAX_BUFFER_SIZE 256
#define FAILURE -1
#define SERVER "server"

// ========================== FUNCTIONS DECLARATIONS ==========================
int initSocket(int portNumber);
int getConnection(int socketNumber);
int callSocket(char *hostName, unsigned short portNum);
int readData(int socketNumber, char *buffer, int n);

// ============================ HELPER FUNCTIONS ============================
int initSocket(int portNumber)
{
    struct sockaddr_in socketData;
    struct hostent* hostData;
    char myHostName[MAX_HOST_NAME_LEN + 1];
    int socketNumber;

    //initialize hostnet
    gethostname(myHostName, MAX_HOST_NAME_LEN);

    if((hostData = gethostbyname(myHostName)) == nullptr)
    {
        return FAILURE;
    }

    memset(&socketData, 0, sizeof(struct sockaddr_in));

    //initialize socketaddr (socket data)
    socketData.sin_family = hostData->h_addrtype;

    memcpy(&socketData.sin_addr, hostData->h_addr, hostData->h_length);
    socketData.sin_port = htons(portNumber);

    //creating socket
    if((socketNumber = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        return FAILURE;
    }

    if (bind(socketNumber, (struct sockaddr *)&socketData, sizeof(struct sockaddr_in)) < 0) {
        close(socketNumber);
        return FAILURE;
    }

    listen(socketNumber, MAX_CLIENTS_TO_LISTEN);
    return socketNumber;
}

int getConnection(int socketNumber){
    int dedicatedSocket; // socket of new connection

    if ((dedicatedSocket = accept(socketNumber, nullptr,nullptr)) < 0){
            return FAILURE;
    }
    return dedicatedSocket;
}

int callSocket(char *hostName, unsigned short portNum){

    struct sockaddr_in socketData;
    struct hostent *hostData;
    int newSocketNumber;

    if ((hostData= gethostbyname (hostName)) == NULL) {
        return FAILURE;
    }

    memset(&socketData,0,sizeof(socketData));
    memcpy((char *)&socketData.sin_addr, hostData->h_addr, hostData->h_length);
    socketData.sin_family = hostData->h_addrtype;
    socketData.sin_port = htons((u_short)portNum);

    if ((newSocketNumber = socket(hostData->h_addrtype,SOCK_STREAM,0)) < 0) {
        return FAILURE;
    }

    if (connect(newSocketNumber, (struct sockaddr *)&socketData , sizeof(socketData)) < 0) {
        close(newSocketNumber);
        return FAILURE;
    }

    return newSocketNumber;
}

int readData(int socketNumber, char *buffer, int n) {
    int alreadyReadByte = 0; // counts bytes read
    int bytesRead = 0; //bytes read this pass

    while (alreadyReadByte < n) { // loop until full buffer
        bytesRead = read(socketNumber, buffer, n-alreadyReadByte);
        if(bytesRead > 0)
        {
            alreadyReadByte += bytesRead;
            buffer += bytesRead;
        }
        if (bytesRead < 1) {
            return FAILURE;
        }
    }
    return alreadyReadByte;
}


// =============================== MAIN PROGRAM ===============================
int main(int argc, char* argv[])
{
    if(argc < 3){
        printf(ERROR_MSG, "insufficient number of arguments\n");
        exit(1);
    }

    char* endUser = argv[1];
    if(strcmp(endUser, "server") != 0 && strcmp(endUser, "client") != 0){
        printf(ERROR_MSG, "incorrect arguments provided to program\n");
        exit(1);
    }
    int portNum = (int)strtol(argv[2], nullptr, 10);
    int mainSocketNumber;
    int clientSocket;

    if(strcmp(endUser, SERVER) == 0)
    {
        mainSocketNumber = initSocket(portNum);
        char serverBuffer[MAX_BUFFER_SIZE];

        while(true){
            while((clientSocket = getConnection(mainSocketNumber)) == FAILURE){
                // pass
            }
            memset(serverBuffer, 0, MAX_BUFFER_SIZE);
            readData(clientSocket, serverBuffer, MAX_BUFFER_SIZE);
            if(system(serverBuffer) != 0){
                printf(ERROR_MSG, "server failed to execute client's command\n");
                exit(1);
            }
        }
    }
    else
    {
        char clientBuffer[MAX_BUFFER_SIZE];
        char* runner = clientBuffer;
        for(int i = 3; i < argc; ++i){
            memcpy(runner, argv[i], strlen(argv[i]));
            runner += strlen(argv[i]) + 1;
            memcpy(runner - 1, " ", 1);
        }
        memcpy(runner, "\0", 1);

        char hostName[MAX_HOST_NAME_LEN];
        gethostname(hostName, MAX_HOST_NAME_LEN);
        clientSocket = callSocket(hostName, portNum);

        if(write(clientSocket, clientBuffer, MAX_BUFFER_SIZE) == FAILURE){
            printf(ERROR_MSG, "client failed to send command to server\n");
            exit(1);
        }
    }
}
