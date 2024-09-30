#include "Tracker.h"

// Constructor
Tracker::Tracker(const std::string &ip, int port) : serverIP(ip), serverPort(port), isRunning(true)
{
    init();
}

// Method to initialize the tracker
void Tracker::init()
{
    WSAData wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        handleError("WSAStartup failed");
    }

    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd == INVALID_SOCKET)
    {
        handleError("Socket creation failed!");
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(serverIP.c_str()); // Bind to the specified IP address
    serverAddr.sin_port = htons(serverPort);

    if (bind(serverFd, (sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        handleError("Bind failed!");
    }

    if (listen(serverFd, SOMAXCONN) == SOCKET_ERROR)
    {
        handleError("Listen failed!");
    }
}

// Method to handle errors
void Tracker::handleError(const std::string &msg)
{
    std::cerr << msg << std::endl;
    WSACleanup();
    exit(1);
}

// Modified start method
void Tracker::start()
{
    std::cout << "Tracker started on " << serverIP << ":" << serverPort << "!" << std::endl;

    while (isRunning) // Check if the server is still running
    {
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientFd = accept(serverFd, (sockaddr *)&clientAddr, &clientAddrLen);
        if (clientFd == INVALID_SOCKET)
        {
            if (!isRunning)
                break; // Exit the loop when shutting down
            std::cerr << "Accept failed!" << std::endl;
            continue;
        }

        // Create a new thread for the client
        clientThreads.emplace_back(&Tracker::handleClient, this, clientFd, clientAddr);
    }

    std::cout << "Tracker is no longer accepting connections." << std::endl;
}

// Method to handle a client
void Tracker::handleClient(SOCKET clientFd, sockaddr_in clientAddr)
{
    std::cout << "Client connected from " << inet_ntoa(clientAddr.sin_addr) << ":" << ntohs(clientAddr.sin_port) << "!" << std::endl;

    char buffer[4096];
    int bytesReceived;

    // Keep the connection open and listen for multiple requests from the client
    while (true)
    {
        // Receive data from the client
        bytesReceived = recv(clientFd, buffer, sizeof(buffer) - 1, 0);

        // Check for client disconnect or receive failure
        if (bytesReceived <= 0)
        {
            if (bytesReceived == 0)
            {
                std::cout << "Client disconnected!" << std::endl;
            }
            else
            {
                std::cerr << "Receive failed!" << std::endl;
            }
            break; // Exit the loop and close the connection
        }

        // Null-terminate the buffer to convert it to a string
        buffer[bytesReceived] = '\0';
        std::string request(buffer);

        // Process the received request
        processRequest(clientFd, request, clientAddr);
    }

    // Clean up: close the connection when the loop ends
    closesocket(clientFd);
    std::cout << "Client connection closed!" << std::endl;
}

// Method to process the client's request
void Tracker::processRequest(SOCKET clientFd, const std::string &request, sockaddr_in clientAddr)
{
    size_t spacePos = request.find(" ");
    if (spacePos == std::string::npos)
    {
        send(clientFd, "Invalid command format", 22, 0);
        return;
    }

    std::string command = request.substr(0, spacePos);
    std::string args = request.substr(spacePos + 1);

    if (command == "announce")
    {
        size_t colonPos = args.find(":");
        if (colonPos == std::string::npos)
        {
            send(clientFd, "Invalid announce format", 22, 0);
            return;
        }
        std::string filename = args.substr(0, colonPos);
        std::string peerPort = args.substr(colonPos + 1);
        announcePeer(clientFd, filename, peerPort, clientAddr);
    }
    else if (command == "getpeers")
    {
        std::string filename = args;
        sendPeersList(clientFd, filename);
    }
    else
    {
        send(clientFd, "Unknown command", 15, 0);
    }
}

// Method to handle "announce" requests from peers
void Tracker::announcePeer(SOCKET clientFd, const std::string &filename, const std::string &peerPort, sockaddr_in clientAddr)
{
    std::lock_guard<std::mutex> lock(mtx);
    std::string peerIP = inet_ntoa(clientAddr.sin_addr);
    int port = std::stoi(peerPort); // Receive the port from peer
    if (filePeers.find(filename) == filePeers.end())
    {
        filePeers[filename] = std::vector<std::pair<std::string, int>>();
    }
    for (const auto &peer : filePeers[filename])
    {
        if (peer.first == peerIP && peer.second == port)
        {
            std::string response = "Peer already registered";
            send(clientFd, response.c_str(), response.size(), 0);
            return;
        }
    }
    filePeers[filename].emplace_back(peerIP, port);
    std::cout << "Peer announced: " << peerIP << ":" << port << " for file " << filename << std::endl;
    std::string response = "Peer registered";
    send(clientFd, response.c_str(), response.size(), 0);
}

// Method to send a list of peers for a requested file
void Tracker::sendPeersList(SOCKET clientFd, const std::string &filename)
{
    std::lock_guard<std::mutex> lock(mtx);

    if (filePeers.find(filename) != filePeers.end())
    {
        std::string peersList;
        for (const auto &peer : filePeers[filename])
        {
            peersList += peer.first + ":" + std::to_string(peer.second) + "\n";
        }
        send(clientFd, peersList.c_str(), peersList.size(), 0);
    }
    else
    {
        std::string response = "No peers available for this file";
        send(clientFd, response.c_str(), response.size(), 0);
    }
}

// Modified shutdown method
void Tracker::shutdown()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (hasShutdown)
        return;         // Prevent double shutdown
    hasShutdown = true; // Mark as shut down

    std::cout << "Shutting down tracker..." << std::endl;
    isRunning = false; // Set the running flag to false

    // Attempt to close the server socket to wake up any blocking accept call
    closesocket(serverFd);

    // Wait for all client threads to finish
    for (std::thread &clientThread : clientThreads)
    {
        if (clientThread.joinable())
        {
            clientThread.join();
        }
    }

    WSACleanup();
    std::cout << "Tracker shut down gracefully." << std::endl;
}

// Method to get a list of peers for a requested file
std::vector<std::pair<std::string, int>> Tracker::getPeers(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::pair<std::string, int>> peersList;
    if (filePeers.find(filename) != filePeers.end())
    {
        peersList = filePeers[filename]; // Get the list of peers
    }
    return peersList;
}

// Getter for Tracker port
int Tracker::getTrackerPort()
{
    return serverPort;
}

// Method to accept a peer connection
SOCKET Tracker::acceptPeerConnection()
{
    sockaddr_in clientAddr;
    int clientAddrLen = sizeof(clientAddr);
    SOCKET clientFd = accept(serverFd, (sockaddr *)&clientAddr, &clientAddrLen);

    if (clientFd == INVALID_SOCKET)
    {
        std::cerr << "Accept failed!" << std::endl;
    }
    return clientFd;
}

// Destructor
Tracker::~Tracker()
{
    for (std::thread &clientThread : clientThreads)
    {
        if (clientThread.joinable())
        {
            clientThread.join();
        }
    }
    shutdown();
}

int main()
{
    std::string trackerIP = "192.168.172.183"; // Change this to your desired IP
    int trackerPort = 9999;                    // Change this to your desired port

    Tracker tracker(trackerIP, trackerPort);

    // Start the tracker
    std::cout << "Starting tracker on " << trackerIP << ":" << trackerPort << "..." << std::endl;
    std::thread trackerThread(&Tracker::start, &tracker);

    // Wait for user input to shut down
    std::cout << "Press Enter to shut down the tracker..." << std::endl;
    std::cin.get();

    tracker.shutdown();

    trackerThread.join(); // Ensure the tracker thread exits properly
    std::cout << "Tracker shut down." << std::endl;

    return 0;
}
