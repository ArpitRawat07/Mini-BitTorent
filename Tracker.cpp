#include "Tracker.h"

// Constructor for the Tracker class
// Initializes the tracker with a specified IP and port, and starts the server
Tracker::Tracker(const std::string &ip, int port) : serverIP(ip), serverPort(port), isRunning(true)
{
    init(); // Call to initialize the tracker (socket setup, binding, etc.)
}

// Method to initialize the tracker
// Sets up the server socket, binds it to the specified IP and port, and starts listening for connections
void Tracker::init()
{
    // Initialize Winsock (Windows Socket API)
    // The MAKEWORD(2, 2) specifies that version 2.2 of Winsock is desired
    WSAData wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        handleError("WSAStartup failed"); // Error handling for Winsock initialization failure
    }

    // Create a socket to listen for TCP connections (AF_INET for IPv4, SOCK_STREAM for TCP)
    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd == INVALID_SOCKET)
    {
        handleError("Socket creation failed!"); // Error handling for socket creation failure
    }

    // Structure to specify the server's IP address and port number
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;                          // Set address family to IPv4
    serverAddr.sin_addr.s_addr = inet_addr(serverIP.c_str()); // Convert IP address string to binary form
    serverAddr.sin_port = htons(serverPort);                  // Convert port number to network byte order

    // Bind the socket to the specified IP address and port
    if (bind(serverFd, (sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        handleError("Bind failed!"); // Error handling for bind failure
    }

    // Start listening for incoming connections with a maximum connection backlog (SOMAXCONN)
    if (listen(serverFd, SOMAXCONN) == SOCKET_ERROR)
    {
        handleError("Listen failed!"); // Error handling for listen failure
    }
}

// Method to handle errors
// Outputs an error message, cleans up Winsock resources, and terminates the program
void Tracker::handleError(const std::string &msg)
{
    std::cerr << msg << std::endl; // Output the error message to the console
    WSACleanup();                  // Clean up Winsock resources
    exit(1);                       // Terminate the program with an error status
}

// Modified start method to accept connections and handle clients in separate threads
void Tracker::start()
{
    // Inform that the tracker has started and display the IP and port it's bound to
    std::cout << "Tracker started on " << serverIP << ":" << serverPort << "!" << std::endl;

    // Main server loop, running until the server is stopped
    while (isRunning) // Check if the server is still running
    {
        sockaddr_in clientAddr; // Structure to hold client address information
        int clientAddrLen = sizeof(clientAddr);

        // Accept an incoming client connection
        SOCKET clientFd = accept(serverFd, (sockaddr *)&clientAddr, &clientAddrLen);
        if (clientFd == INVALID_SOCKET) // Error handling for failed connection
        {
            if (!isRunning)
                break; // Exit the loop if the server is shutting down
            std::cerr << "Accept failed!" << std::endl;
            continue; // Continue the loop to handle more connections
        }

        // Start a new thread to handle the client connection, passing the client socket and address
        clientThreads.emplace_back(&Tracker::handleClient, this, clientFd, clientAddr);
    }

    // Inform that the server is no longer accepting connections
    std::cout << "Tracker is no longer accepting connections." << std::endl;
}

// Method to handle communication with a connected client
void Tracker::handleClient(SOCKET clientFd, sockaddr_in clientAddr)
{
    // Display the client's IP address and port
    std::cout << "Client connected from " << inet_ntoa(clientAddr.sin_addr) << ":" << ntohs(clientAddr.sin_port) << "!" << std::endl;

    char buffer[4096]; // Buffer to store incoming data from the client
    int bytesReceived; // Variable to hold the number of bytes received

    // Keep the connection open and allow multiple requests from the same client
    while (true)
    {
        // Receive data from the client
        bytesReceived = recv(clientFd, buffer, sizeof(buffer) - 1, 0);

        // Check if the client has disconnected or if an error occurred during receiving
        if (bytesReceived <= 0)
        {
            if (bytesReceived == 0) // Client closed the connection
            {
                std::cout << "Client disconnected!" << std::endl;
            }
            else
            {
                std::cerr << "Receive failed!" << std::endl; // Error during receiving
            }
            break; // Exit the loop and close the connection
        }

        // Null-terminate the received data to convert it into a string
        buffer[bytesReceived] = '\0';
        std::string request(buffer);

        // Process the request received from the client
        processRequest(clientFd, request, clientAddr);
    }

    // Close the client socket when done
    closesocket(clientFd);
    std::cout << "Client connection closed!" << std::endl;
}

// Method to process and respond to client requests
void Tracker::processRequest(SOCKET clientFd, const std::string &request, sockaddr_in clientAddr)
{
    // Find the position of the first space to separate the command from its arguments
    size_t spacePos = request.find(" ");
    if (spacePos == std::string::npos) // If no space is found, it's an invalid command
    {
        send(clientFd, "Invalid command format", 22, 0);
        return;
    }

    // Extract the command and the arguments from the request
    std::string command = request.substr(0, spacePos);
    std::string args = request.substr(spacePos + 1);

    // Handle "announce" command from peers (to register the peer and file info)
    if (command == "announce")
    {
        size_t colonPos = args.find(":");  // Find the colon separating the filename and port
        if (colonPos == std::string::npos) // Invalid format if colon is missing
        {
            send(clientFd, "Invalid announce format", 22, 0);
            return;
        }

        // Extract the filename and peer's port from the arguments
        std::string filename = args.substr(0, colonPos);
        std::string peerPort = args.substr(colonPos + 1);

        // Register the peer for the given file
        announcePeer(clientFd, filename, peerPort, clientAddr);
    }
    // Handle "getpeers" command from clients (to retrieve the list of peers for a file)
    else if (command == "getpeers")
    {
        std::string filename = args;
        sendPeersList(clientFd, filename); // Send the list of peers for the requested file
    }
    else // Unknown command handling
    {
        send(clientFd, "Unknown command", 15, 0);
    }
}

// Method to handle "announce" requests from peers (registers peer for a specific file)
void Tracker::announcePeer(SOCKET clientFd, const std::string &filename, const std::string &peerPort, sockaddr_in clientAddr)
{
    std::lock_guard<std::mutex> lock(mtx); // Lock mutex to ensure thread-safe access to shared data

    std::string peerIP = inet_ntoa(clientAddr.sin_addr); // Get the peer's IP address
    int port = std::stoi(peerPort);                      // Convert peer's port from string to integer

    // Check if the file already has registered peers
    if (filePeers.find(filename) == filePeers.end())
    {
        filePeers[filename] = std::vector<std::pair<std::string, int>>(); // Initialize a new entry for the file
    }

    // Check if the peer is already registered for the file
    for (const auto &peer : filePeers[filename])
    {
        if (peer.first == peerIP && peer.second == port) // If the peer is already registered
        {
            std::string response = "Peer already registered";
            send(clientFd, response.c_str(), response.size(), 0);
            return;
        }
    }

    // Register the peer's IP and port for the file
    filePeers[filename].emplace_back(peerIP, port);
    std::cout << "Peer announced: " << peerIP << ":" << port << " for file " << filename << std::endl;

    // Send confirmation to the peer
    std::string response = "Peer registered";
    send(clientFd, response.c_str(), response.size(), 0);
}

// Method to send a list of peers for a requested file
void Tracker::sendPeersList(SOCKET clientFd, const std::string &filename)
{
    std::lock_guard<std::mutex> lock(mtx); // Lock mutex to ensure thread-safe access to shared data

    // Check if there are peers registered for the requested file
    if (filePeers.find(filename) != filePeers.end())
    {
        std::string peersList;
        // Concatenate the IP and port of each peer into a list
        for (const auto &peer : filePeers[filename])
        {
            peersList += peer.first + ":" + std::to_string(peer.second) + "\n";
        }
        send(clientFd, peersList.c_str(), peersList.size(), 0); // Send the peer list to the client
    }
    else
    {
        // If no peers are available for the file, notify the client
        std::string response = "No peers available for this file";
        send(clientFd, response.c_str(), response.size(), 0);
    }
}

// Modified shutdown method to handle graceful shutdown of the tracker
void Tracker::shutdown()
{
    std::lock_guard<std::mutex> lock(mtx); // Lock the mutex to protect shared resources
    if (hasShutdown)                       // Check if shutdown has already been called
        return;                            // Prevent double shutdown

    hasShutdown = true; // Mark as shut down to prevent further shutdown attempts

    std::cout << "Shutting down tracker..." << std::endl; // Notify about shutdown process
    isRunning = false;                                    // Set the running flag to false to stop accepting new connections

    // Attempt to close the server socket to wake up any blocking accept call
    closesocket(serverFd); // Close the server socket to unblock any pending accept calls

    // Wait for all client threads to finish
    for (std::thread &clientThread : clientThreads) // Iterate through all client threads
    {
        if (clientThread.joinable()) // Check if the thread is joinable
        {
            clientThread.join(); // Wait for the thread to finish execution
        }
    }

    WSACleanup();                                              // Clean up the Windows Sockets API
    std::cout << "Tracker shut down gracefully." << std::endl; // Notify about successful shutdown
}

// Method to get a list of peers for a requested file
std::vector<std::pair<std::string, int>> Tracker::getPeers(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(mtx);              // Lock the mutex to protect shared resources
    std::vector<std::pair<std::string, int>> peersList; // Vector to store the list of peers
    if (filePeers.find(filename) != filePeers.end())    // Check if the file exists in the map
    {
        peersList = filePeers[filename]; // Get the list of peers associated with the file
    }
    return peersList; // Return the list of peers
}

// Method to get all files and their associated peers
std::map<std::string, std::vector<std::pair<std::string, int>>> Tracker::getFileAndPeers()
{
    return filePeers; // Return the map containing files and their associated peers
}

// Getter for the Tracker port
int Tracker::getTrackerPort()
{
    return serverPort; // Return the server port number
}

// Method to accept a peer connection
SOCKET Tracker::acceptPeerConnection()
{
    sockaddr_in clientAddr;                                                      // Structure to hold client address information
    int clientAddrLen = sizeof(clientAddr);                                      // Length of the client address structure
    SOCKET clientFd = accept(serverFd, (sockaddr *)&clientAddr, &clientAddrLen); // Accept a new connection

    if (clientFd == INVALID_SOCKET) // Check if the accept call failed
    {
        std::cerr << "Accept failed!" << std::endl; // Log an error message
    }
    return clientFd; // Return the client socket
}

// Destructor to clean up resources
Tracker::~Tracker()
{
    // Wait for all client threads to finish
    for (std::thread &clientThread : clientThreads)
    {
        if (clientThread.joinable()) // Check if the thread is joinable
        {
            clientThread.join(); // Wait for the thread to finish execution
        }
    }
    shutdown(); // Call shutdown method to clean up resources
}

int main()
{
    // Set tracker IP and port configuration
    std::string trackerIP;
    int trackerPort;
    std::cout << "Enter the tracker IP address: \n";
    std::cin >> trackerIP; // Input the tracker IP address
    std::cout << "Enter the tracker port number: \n";
    std::cin >> trackerPort; // Input the tracker port number

    std::cin.ignore(); // Ignore the newline character left in the input buffer

    // Create an instance of the Tracker with the specified IP and port
    Tracker tracker(trackerIP, trackerPort);

    // Start the tracker in a separate thread
    std::cout << "Starting tracker on " << trackerIP << ":" << trackerPort << "..." << std::endl;
    std::thread trackerThread(&Tracker::start, &tracker); // Start the tracker thread

    // Wait for user input to shut down the tracker
    std::cout << "Press Enter to shut down the tracker..." << std::endl;
    std::cin.get(); // Wait for the user to press Enter

    tracker.shutdown(); // Call shutdown method to gracefully shut down the tracker

    trackerThread.join();                           // Ensure the tracker thread exits properly
    std::cout << "Tracker shut down." << std::endl; // Notify that the tracker has shut down

    return 0; // Exit the program successfully
}
