#include "Peer.h"

// Mutex to ensure thread-safe console output
std::mutex consoleMutex;

// Thread-safe function to print messages to the console
void safePrint(const std::string &message)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cout << message << std::endl; // Output message to console
}

// Thread-safe function to print error messages to the console
void safeErrorPrint(const std::string &message)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cerr << message << std::endl; // Output error message to standard error
}

// Peer constructor: Initializes the peer with necessary details (IP, port, directories, etc.)
Peer::Peer(const std::string &ip, int port, const std::string &id, const std::string &trackerIp, int trackerPort,
           const std::string &uploadDir, const std::string &downloadDir)
    : peerIP(ip), peerPort(port), peerId(id), trackerIP(trackerIp), trackerPort(trackerPort),
      uploadPath(uploadDir), downloadPath(downloadDir), fileSize(0), trackerSocket(INVALID_SOCKET), isRunning(true)
{
    // Initialize Winsock (required for socket operations on Windows)
    WSAData wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) // Error handling for Winsock initialization
    {
        safeErrorPrint("WSAStartup failed!"); // Print error message
        // Optional: Handle the error, potentially throwing an exception or marking the object as unusable
    }

    // Create a socket for the peer
    peerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (peerSocket == INVALID_SOCKET) // Error handling for socket creation
    {
        safeErrorPrint("Socket creation failed!"); // Print error message
        // Optional: Handle the error, potentially throwing an exception or marking the object as unusable
    }

    // Set up the peer address (IP and port)
    sockaddr_in peerAddr;
    peerAddr.sin_family = AF_INET;                        // Use IPv4
    peerAddr.sin_addr.s_addr = inet_addr(peerIP.c_str()); // Convert peer IP from string to network address
    peerAddr.sin_port = htons(peerPort);                  // Convert peer port from host to network byte order

    // Bind the socket to the peer's address
    if (bind(peerSocket, (sockaddr *)&peerAddr, sizeof(peerAddr)) == SOCKET_ERROR)
    {
        safeErrorPrint("Bind failed!"); // Error handling for bind failure
        // Optional: Handle the error, potentially throwing an exception or marking the object as unusable
    }

    // Start listening for incoming connections on the socket
    if (listen(peerSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        safeErrorPrint("Listen failed!"); // Error handling for listen failure
        // Optional: Handle the error, potentially throwing an exception or marking the object as unusable
    }
}

// Peer destructor: Cleans up resources when the peer object is destroyed
Peer::~Peer()
{
    isRunning = false; // Signal that the peer should stop running

    // Close the peer socket to stop accepting connections
    if (peerSocket != INVALID_SOCKET)
    {
        closesocket(peerSocket);     // Close the listening socket
        peerSocket = INVALID_SOCKET; // Invalidate the socket
    }

    disconnectAllPeers(); // Ensure all active peer connections are safely closed

    // Join (wait for completion of) all threads handling peer connections
    for (std::thread &t : otherPeerThreads)
    {
        if (t.joinable()) // Check if the thread can be joined
        {
            t.join(); // Wait for the thread to finish
        }
    }

    WSACleanup(); // Clean up Winsock (release resources)
}

// Method to safely disconnect all connected peers
void Peer::disconnectAllPeers()
{
    std::lock_guard<std::mutex> lock(mtx); // Ensure thread-safe access to the list of connected peers

    // Iterate through all connected peers and close their sockets
    for (auto &peer : connectedPeers)
    {
        closesocket(peer.socket); // Close the socket for each peer
    }

    connectedPeers.clear(); // Clear the list of connected peers
}

// Method to start the peer and begin accepting connections
void Peer::start()
{
    safePrint("Peer started on " + peerIP + ":" + std::to_string(peerPort)); // Log the peer's IP and port

    // Main loop to accept incoming peer connections
    while (isRunning) // Continue running as long as the peer is active
    {
        sockaddr_in clientAddr; // Structure to hold the address of the connecting client
        int clientAddrLen = sizeof(clientAddr);

        // Accept an incoming client connection
        SOCKET clientFd = accept(peerSocket, (sockaddr *)&clientAddr, &clientAddrLen);
        if (clientFd == INVALID_SOCKET) // Error handling for failed connections
        {
            if (!isRunning)
                break;                        // Exit the loop if the peer is shutting down
            safeErrorPrint("Accept failed!"); // Log the failure
            continue;                         // Continue to accept the next connection
        }

        {
            std::lock_guard<std::mutex> lock(mtx); // Ensure thread-safe access to the list of threads

            // Check if the client is already handled by an existing thread
            if (std::find_if(otherPeerThreads.begin(), otherPeerThreads.end(),
                             [clientFd](std::thread &t)
                             { return t.native_handle() == clientFd; }) == otherPeerThreads.end())
            {
                // Start a new thread to handle the connection to the peer
                otherPeerThreads.emplace_back(&Peer::handlePeer, this, clientFd, clientAddr);
            }
        }
    }

    safePrint("Peer is no longer accepting connections."); // Log that the peer has stopped accepting connections
}

// Method to handle a peer connection
void Peer::handlePeer(SOCKET clientFd, sockaddr_in clientAddr)
{
    char ipBuffer[INET_ADDRSTRLEN];                                       // Buffer to store the peer's IP address as a string
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuffer, sizeof(ipBuffer)); // Convert the peer's IP address to string format
    int clientPort = ntohs(clientAddr.sin_port);                          // Convert the peer's port from network byte order to host byte order

    // Log the connection of the peer (IP and port)
    safePrint("Peer connected: " + std::string(ipBuffer) + ":" + std::to_string(clientPort));

    {
        std::lock_guard<std::mutex> lock(peerMutex); // Lock to ensure thread-safe access to connectedPeers

        // Check if the peer is already in the connectedPeers list
        if (std::find_if(connectedPeers.begin(), connectedPeers.end(),
                         [clientFd](const PeerInfo &peer)
                         { return peer.socket == clientFd; }) == connectedPeers.end())
        {
            // Add the peer to the connectedPeers list if not already present
            connectedPeers.emplace_back(PeerInfo{std::string(ipBuffer), clientPort, clientFd});
        }
    }

    // Communication loop: Continuously receive messages from the peer
    while (true)
    {
        char buffer[4096]; // Buffer to store the received message
        int bytesReceived;

        // Receive data from the peer
        bytesReceived = recv(clientFd, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived > 0)
        {
            // Null-terminate the received data to make it a valid string
            buffer[bytesReceived] = '\0';
            std::string message(buffer); // Convert the received data to a string

            // Log the received message
            safePrint("Received message from peer: " + message);

            // Process the message received from the peer
            processMessage(message, clientFd);
        }
        else
        {
            // If no data is received, it means the connection is lost
            safeErrorPrint("Connection lost with peer: " + std::string(ipBuffer) + ":" + std::to_string(clientPort));
            break; // Exit the communication loop
        }
    }

    {
        std::lock_guard<std::mutex> lock(peerMutex); // Lock to ensure thread-safe closing of the socket
        closesocket(clientFd);                       // Close the peer's socket after communication ends
    }

    // Log that the peer has disconnected
    safePrint("Peer disconnected: " + std::string(ipBuffer) + ":" + std::to_string(clientPort));

    // Remove the disconnected peer from the connectedPeers list
    connectedPeers.erase(std::remove_if(connectedPeers.begin(), connectedPeers.end(),
                                        [clientFd](const PeerInfo &peer)
                                        { return peer.socket == clientFd; }),
                         connectedPeers.end()); // Remove the peer with the matching socket
}

// Method to connect to the tracker
bool Peer::connectToTracker()
{
    // Create a socket for connecting to the tracker server (AF_INET for IPv4, SOCK_STREAM for TCP)
    trackerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (trackerSocket == INVALID_SOCKET) // Check if socket creation failed
    {
        // Log an error if the socket could not be created
        safeErrorPrint("Socket creation for tracker failed!");
        return false; // Return false to indicate failure
    }

    // Set up the address structure for the tracker connection
    sockaddr_in trackerAddr;
    memset(&trackerAddr, 0, sizeof(trackerAddr));               // Zero out the address structure for initialization
    trackerAddr.sin_family = AF_INET;                           // Specify the address family (IPv4)
    trackerAddr.sin_port = htons(trackerPort);                  // Set the tracker port number, converting to network byte order
    trackerAddr.sin_addr.s_addr = inet_addr(trackerIP.c_str()); // Convert the tracker IP from string format to network format

    // Attempt to establish a connection to the tracker
    if (connect(trackerSocket, (sockaddr *)&trackerAddr, sizeof(trackerAddr)) == SOCKET_ERROR)
    {
        // Log an error if the connection to the tracker fails
        safeErrorPrint("Failed to connect to tracker at: " + trackerIP + ":" + std::to_string(trackerPort));
        closesocket(trackerSocket);     // Close the socket if the connection failed
        trackerSocket = INVALID_SOCKET; // Reset the tracker socket state to indicate failure
        return false;                   // Return false to indicate that the connection failed
    }

    // Log a successful connection to the tracker
    safePrint("Successfully connected to tracker at: " + trackerIP + ":" + std::to_string(trackerPort));
    return true; // Return true to indicate that the connection was successful
}

// Method to announce to the tracker
bool Peer::announceToTracker()
{
    // Ensure the upload path is valid
    if (!fs::exists(uploadPath) || !fs::is_directory(uploadPath))
    {
        safeErrorPrint("Upload path does not exist or is not a directory!"); // Print error if invalid
        return false;                                                        // Return false to indicate failure
    }

    std::lock_guard<std::mutex> fileLock(fileMutex); // Lock file operations to ensure thread safety

    // Iterate through all files in the upload directory
    for (const auto &entry : fs::directory_iterator(uploadPath))
    {
        if (entry.is_regular_file()) // Check if the entry is a regular file
        {
            // Get the file name from the entry
            std::string fileName = entry.path().filename().string();

            {
                std::lock_guard<std::mutex> lock(peerMutex); // Lock access to filePeers to prevent data races
                // If the peer is not already in the filePeers for this filename
                if (std::find_if(filePeers[fileName].begin(), filePeers[fileName].end(),
                                 [peerPort = peerPort](const std::pair<std::string, int> &peer)
                                 { return peer.second == peerPort; }) == filePeers[fileName].end())
                {
                    // Add the peer's IP and port to the filePeers map for this file
                    filePeers[fileName].emplace_back(peerIP, peerPort);
                }
            }

            // Construct the announce message for the file
            std::string announceMessage = "announce " + fileName + ":" + std::to_string(peerPort);

            // Synchronize sending the announce message
            int bytesSent = send(trackerSocket, announceMessage.c_str(), announceMessage.size(), 0);

            // Check for send errors
            if (bytesSent == SOCKET_ERROR)
            {
                safeErrorPrint("Failed to send announce message to tracker for file: " + fileName);
                return false; // Return false to indicate failure
            }

            safePrint("Announced to tracker: " + announceMessage); // Log the announce message

            // Receive the response from the tracker
            char buffer[4096];                                                      // Buffer to hold the incoming data
            int bytesReceived = recv(trackerSocket, buffer, sizeof(buffer) - 1, 0); // Receive data

            if (bytesReceived > 0) // Check if data was received
            {
                buffer[bytesReceived] = '\0';                                         // Null-terminate the response
                std::string response(buffer);                                         // Convert buffer to string
                safePrint("Tracker response for file " + fileName + ": " + response); // Log response
            }
            else
            {
                safeErrorPrint("Failed to receive response from tracker for file: " + fileName);
                return false; // Return false to indicate failure
            }
        }
    }

    return true; // Successfully announced all files
}

// Method to request peers from the tracker for a specific file
void Peer::requestPeers(const std::string &requiredFile)
{
    // Construct the request message to get peers for the specified file
    std::string requestMessage = "getpeers " + requiredFile;

    send(trackerSocket, requestMessage.c_str(), requestMessage.size(), 0); // Send request to tracker

    safePrint("Requested peers list for file '" + requiredFile + "' from tracker."); // Log request

    char buffer[4096];                                                      // Buffer to hold incoming data
    int bytesReceived = recv(trackerSocket, buffer, sizeof(buffer) - 1, 0); // Receive data from tracker

    if (bytesReceived > 0) // Check if data was received
    {
        buffer[bytesReceived] = '\0'; // Null-terminate the received data
        std::string response(buffer); // Convert buffer to string

        // Check if the response indicates no available peers
        if (response.find("No peers available for this file") != std::string::npos)
        {
            safePrint("Tracker response: " + response); // Log tracker response
            return;                                     // No peers are available, exit the function
        }

        safePrint("Received peers list: " + response); // Log received peers list

        // Split the response by newline to get individual peer IP:port pairs
        std::stringstream ss(response); // Create a stringstream for parsing the response
        std::string peerEntry;          // String to hold each peer entry

        // Connect to each peer in the list
        while (std::getline(ss, peerEntry, '\n')) // Read each line (peer entry)
        {
            size_t colonPos = peerEntry.find(":"); // Find position of the colon
            if (colonPos != std::string::npos)     // Check if a colon was found
            {
                // Extract the peer IP and port
                std::string peerIP = peerEntry.substr(0, colonPos);       // Get IP
                int peerPort = std::stoi(peerEntry.substr(colonPos + 1)); // Get port

                // Add the peer to the filePeers map
                {
                    std::lock_guard<std::mutex> lock(mtx); // Lock access to filePeers for thread safety
                    // If the peer is not already present in the filePeers for requiredFile
                    if (std::find_if(filePeers[requiredFile].begin(), filePeers[requiredFile].end(),
                                     [peerPort](const std::pair<std::string, int> &peer)
                                     { return peer.second == peerPort; }) == filePeers[requiredFile].end())
                    {
                        // Add the peer's IP and port to the filePeers map for this file
                        filePeers[requiredFile].emplace_back(peerIP, peerPort);
                    }
                }

                // Attempt to connect to the peer
                connectToPeer(peerIP, peerPort); // Connect to the peer using its IP and port
            }
        }
    }
    else
    {
        safeErrorPrint("Failed to receive peers list from tracker!"); // Log error if no data received
    }
}

// Method to connect to a peer using the peer's IP and port
bool Peer::connectToPeer(const std::string &incomingPeerIP, int incomingPeerPort)
{
    // Log the attempt to connect to the peer
    safePrint("Connecting to peer: " + incomingPeerIP + ":" + std::to_string(incomingPeerPort));

    // Check if the peer is already connected
    {
        std::lock_guard<std::mutex> lock(mtx); // Lock the connectedPeers list for thread-safe access
        for (const auto &peerInfo : connectedPeers)
        {
            // If the peer IP and port match and the socket is valid, the peer is already connected
            if (peerInfo.ip == incomingPeerIP && peerInfo.port == incomingPeerPort && peerInfo.socket != INVALID_SOCKET)
            {
                safeErrorPrint("Peer " + incomingPeerIP + ":" + std::to_string(incomingPeerPort) + " is already connected.");
                return true; // Peer is already connected, no need to reconnect
            }
        }
    }

    // Create a new socket for the incoming peer connection
    SOCKET incomingPeerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (incomingPeerSocket == INVALID_SOCKET)
    {
        // Log an error if socket creation fails
        safeErrorPrint("Socket creation failed!");
        return false; // Return false to indicate failure
    }

    // Set up the address structure for the incoming peer
    sockaddr_in peerAddr;
    memset(&peerAddr, 0, sizeof(peerAddr));                       // Zero out the memory for initialization
    peerAddr.sin_family = AF_INET;                                // Set the address family to IPv4
    peerAddr.sin_port = htons(incomingPeerPort);                  // Convert the peer port to network byte order
    peerAddr.sin_addr.s_addr = inet_addr(incomingPeerIP.c_str()); // Convert peer IP from string to network format

    // Check if the IP address is valid
    if (peerAddr.sin_addr.s_addr == INADDR_NONE)
    {
        // Log an error if the IP address is invalid
        safeErrorPrint("Invalid peer IP address: " + incomingPeerIP);
        closesocket(incomingPeerSocket); // Close the socket as it's not valid
        return false;                    // Return false due to invalid IP
    }

    // Attempt to connect to the peer using the specified IP and port
    if (connect(incomingPeerSocket, (sockaddr *)&peerAddr, sizeof(peerAddr)) == SOCKET_ERROR)
    {
        // If connection fails, get the error code and log it
        int errorCode = WSAGetLastError(); // Get the last socket error on Windows
        safeErrorPrint("Failed to connect to peer: " + incomingPeerIP + ":" + std::to_string(incomingPeerPort) +
                       ". Error code: " + std::to_string(errorCode));
        closesocket(incomingPeerSocket); // Close the socket as the connection failed
        return false;                    // Return false to indicate connection failure
    }

    // Store the connected peer's information (IP, port, and socket) in the connectedPeers list
    {
        std::lock_guard<std::mutex> lock(mtx); // Lock the connectedPeers list for thread-safe modification
        // Check if the peer is not already present in connectedPeers
        if (std::find_if(connectedPeers.begin(), connectedPeers.end(),
                         [incomingPeerIP, incomingPeerPort](const PeerInfo &peer)
                         { return peer.ip == incomingPeerIP && peer.port == incomingPeerPort; }) == connectedPeers.end())
        {
            // Add the peer to the connectedPeers list if it doesn't already exist
            connectedPeers.emplace_back(PeerInfo{incomingPeerIP, incomingPeerPort, incomingPeerSocket});
        }
    }

    // Send a handshake message to the newly connected peer, providing this peer's listening port and socket
    sendHandshake(incomingPeerSocket, peerPort, peerSocket);

    // Start a new listener thread to receive incoming messages from the peer
    std::thread listenerThread(&Peer::listenForMessages, this, incomingPeerSocket);
    listenerThread.detach(); // Detach the thread so it runs independently without blocking

    return true; // Return true to indicate successful connection to the peer
}

// Method to send a message to a connected peer
void Peer::sendMessage(const std::string &message, SOCKET connectedPeerSocket)
{
    // Send the message to the connected peer's socket
    int result = send(connectedPeerSocket, message.c_str(), message.size(), 0);

    if (result == SOCKET_ERROR)
    {
        // If sending fails, log the error with the corresponding error code
        int errorCode = WSAGetLastError(); // For Windows systems, retrieve the last error
        safeErrorPrint("Failed to send message to peer socket: " + std::to_string(connectedPeerSocket) +
                       "! Error code: " + std::to_string(errorCode));
    }
    else
    {
        // Log successful message transmission
        safePrint("Message sent to peer socket: " + std::to_string(connectedPeerSocket) +
                  " with message: " + message);
    }
}

// Method to receive a message from a peer
std::string Peer::receiveMessage(SOCKET peerSocket)
{
    char buffer[2048];                                                   // Buffer to hold the received message data
    int bytesReceived = recv(peerSocket, buffer, sizeof(buffer) - 1, 0); // Receive data from the peer's socket

    if (bytesReceived > 0)
    {
        buffer[bytesReceived] = '\0'; // Null-terminate the received data to make it a valid C-string
        return std::string(buffer);   // Convert the C-string to a std::string and return it
    }
    else
    {
        // Log an error if the connection is lost or no data is received
        safeErrorPrint("Connection lost with a peer!");
        return ""; // Return an empty string to indicate failure
    }
}

// Listen for incoming messages from a specific peer continuously
void Peer::listenForMessages(SOCKET peerSocket)
{
    char buffer[4096]; // Buffer to hold received data

    // Continuously listen for incoming messages from the connected peer
    while (true)
    {
        // Receive data from the peer's socket
        int bytesReceived = recv(peerSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0';              // Null-terminate the received message
            std::string message(buffer);               // Convert the buffer into a string
            safePrint("Received message: " + message); // Log the received message

            // Process the received message
            processMessage(message, peerSocket);
        }
        else
        {
            // Log an error if the connection is lost
            safeErrorPrint("Connection lost with a peer!");
            break; // Exit the loop if the peer disconnects or an error occurs
        }
    }

    // Cleanup after the peer disconnects
    disconnectPeer(peerSocket);
}

// Method to disconnect a specific peer using its socket
void Peer::disconnectPeer(SOCKET peerSocket)
{
    {
        std::lock_guard<std::mutex> lock(mtx); // Lock the connectedPeers list to ensure thread safety
        closesocket(peerSocket);               // Close the peer's socket

        // Remove the disconnected peer from the connectedPeers list
        connectedPeers.erase(std::remove_if(connectedPeers.begin(), connectedPeers.end(),
                                            [peerSocket](const PeerInfo &peer)
                                            { return peer.socket == peerSocket; }), // Find the peer with the matching socket
                             connectedPeers.end());                                 // Erase the peer from the list
    }

    // Log that the peer has been disconnected
    safePrint("Disconnected peer with socket: " + std::to_string(peerSocket));
}

// Function to request a file from a peer
void Peer::requestFile(const std::string &filename, SOCKET peerSocket)
{
    // Construct a file request message with the filename
    std::string requestMessage = "REQUEST " + filename;
    // Send the request to the peer
    sendMessage(requestMessage, peerSocket);
    // Log the request action
    safePrint("Requested file: " + filename);
}

// Function to acknowledge a file request
void Peer::sendAck(const std::string &filename, SOCKET peerSocket)
{
    // Construct an acknowledgment message for the requested file
    std::string ackMessage = "ACK " + filename;
    // Send the acknowledgment message to the peer
    sendMessage(ackMessage, peerSocket);
    // Log the acknowledgment
    safePrint("Sent ACK for: " + filename);
}

// Method to check if a specific peer is connected based on its IP and port
bool Peer::isPeerConnected(const std::string &peerIP, int peerPort)
{
    std::lock_guard<std::mutex> lock(mtx); // Lock the connectedPeers list for thread-safe access

    // Search for the peer in the connectedPeers list by matching the IP and port
    auto it = std::find_if(connectedPeers.begin(), connectedPeers.end(),
                           [&](const PeerInfo &peer)
                           {
                               return peer.ip == peerIP && peer.port == peerPort;
                           });

    // Return true if the peer is found, false otherwise
    return it != connectedPeers.end();
}

// Send a handshake message after establishing a connection, providing the listening port and socket
void Peer::sendHandshake(SOCKET connectedPeerSocket, int listeningPort, SOCKET listeningSocket)
{
    // Construct the handshake message containing the listening port
    sendMessage("HANDSHAKE " + std::to_string(listeningPort), connectedPeerSocket);
    // No lock is needed for sending the message since we're handling socket communication here
}

// Method to determine the type of an incoming message based on its content
MessageType Peer::getMessageType(const std::string &message)
{
    // Check if the message starts with specific keywords and return the corresponding message type
    if (message.find("REQUEST") == 0)
        return MessageType::REQUEST; // File request
    if (message.find("ACK") == 0)
        return MessageType::ACK; // Acknowledgment of a request
    if (message.find("DATA") == 0)
        return MessageType::DATA; // Data transmission
    if (message.find("COMPLETE") == 0)
        return MessageType::COMPLETE; // Completion of a file transfer
    if (message.find("ERROR") == 0)
        return MessageType::ERROR_; // Error message
    if (message.find("STATUS") == 0)
        return MessageType::STATUS; // Status update
    if (message.find("HANDSHAKE") == 0)
        return MessageType::HANDSHAKE; // Handshake message during connection establishment

    // Return UNKNOWN if no known message type is matched
    return MessageType::UNKNOWN;
}

// Process incoming messages from connected peers
void Peer::processMessage(const std::string &message, SOCKET connectedPeerSocket)
{
    // Extract the IP address and port of the connected peer
    sockaddr_in peerAddr;           // Structure to hold peer address
    int addrLen = sizeof(peerAddr); // Length of the address structure

    // Get the peer's address information
    if (getpeername(connectedPeerSocket, (sockaddr *)&peerAddr, &addrLen) == SOCKET_ERROR)
    {
        // If failed to get peer name, log an error and return
        safeErrorPrint("Failed to get peer name!");
        return;
    }

    // Buffer to hold the IP address as a string
    char ipBuffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peerAddr.sin_addr, ipBuffer, sizeof(ipBuffer)); // Convert IP to string
    int connectedPeerPort = ntohs(peerAddr.sin_port);                   // Convert port to host byte order

    // Log the received message with peer's IP and port
    safePrint("Received message from " + std::string(ipBuffer) + ":" + std::to_string(connectedPeerPort) + " - " + message);

    // Determine the type of the incoming message
    MessageType type = getMessageType(message);

    // Handle the message based on its type
    switch (type)
    {
    case MessageType::REQUEST: // Handling file request messages
    {
        std::lock_guard<std::mutex> lock(fileMutex);        // Lock for thread-safe file operations
        std::string filename = message.substr(8);           // Extract filename from the message
        std::string fullPath = uploadPath + "/" + filename; // Construct full path for the file to upload

        // Send ACK for the file request
        sendMessage("ACK " + filename, connectedPeerSocket);
        std::ifstream file(fullPath, std::ios::binary); // Open the requested file in binary mode

        if (file.is_open()) // Check if the file was successfully opened
        {
            char fileBuffer[1024];                            // Buffer for reading file data
            while (file.read(fileBuffer, sizeof(fileBuffer))) // Read the file in chunks
            {
                std::string dataChunk(fileBuffer, file.gcount());                       // Create a string from the chunk read
                sendMessage("DATA " + filename + " " + dataChunk, connectedPeerSocket); // Send the data chunk to the peer
            }
            // Send any remaining data after the loop
            if (file.gcount() > 0)
            {
                std::string dataChunk(fileBuffer, file.gcount());
                sendMessage("DATA " + filename + " " + dataChunk, connectedPeerSocket); // Send remaining data
            }

            // Wait briefly before notifying the completion of the transfer
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sendMessage("COMPLETE " + filename, connectedPeerSocket); // Notify the peer that the transfer is complete

            file.close();                                     // Close the file
            safePrint("File transfer complete: " + filename); // Log the completion of the file transfer
        }
        else
        {                                                                          // If the file cannot be opened, send an error message
            sendMessage("ERROR File not found: " + filename, connectedPeerSocket); // Send error message to peer
            safeErrorPrint("Failed to open file: " + fullPath);                    // Log the error
        }
        break;
    }
    case MessageType::HANDSHAKE: // Handling handshake messages
    {
        std::string port = message.substr(10);                                                 // Extract the port from the message
        int listeningPort = std::stoi(port);                                                   // Convert the port string to an integer
        safePrint("Received handshake with listening port: " + std::to_string(listeningPort)); // Log the handshake

        // Update the peer info with the received listening port
        {
            std::lock_guard<std::mutex> lock(peerMutex); // Lock the peer list for thread-safe access
            for (auto &peer : connectedPeers)            // Iterate through the connected peers
            {
                // Update the port for the peer with the matching socket
                if (peer.socket == connectedPeerSocket)
                {
                    peer.port = listeningPort; // Set the peer's listening port
                    safePrint("Updated peer " + std::string(ipBuffer) + " with listening port: " + std::to_string(listeningPort));
                    break; // Exit the loop once the peer is found
                }
            }
        }
        break;
    }
    case MessageType::ACK: // Handling acknowledgment messages
    {
        std::string filename = message.substr(4);        // Extract the filename from the message
        safePrint("Received ACK for file: " + filename); // Log the acknowledgment
        break;
    }
    case MessageType::DATA: // Handling incoming data messages
    {
        std::string fileData = message.substr(5); // Extract the file data from the message

        {
            std::lock_guard<std::mutex> lock(fileMutex); // Lock for thread-safe file operations

            if (currentFilename.empty()) // If this is the first DATA message
            {
                // Extract the filename from the incoming data message
                size_t pos = fileData.find(" ");
                if (pos != std::string::npos)
                {
                    std::string filename = fileData.substr(0, pos);           // Get the filename
                    currentFilename = downloadPath + "/" + filename;          // Set the path for downloaded file
                    std::string uploadFilename = uploadPath + "/" + filename; // Set path for another copy in the upload directory

                    // Open the output file in the download directory
                    outputDownloadFile.open(currentFilename);
                    if (!outputDownloadFile.is_open()) // Check if the file was created successfully
                    {
                        safeErrorPrint("Failed to create output file: " + currentFilename); // Log the error
                        return;                                                             // Exit if file creation fails
                    }
                    safePrint("Created output file in download location: " + currentFilename); // Log successful file creation

                    // Open the output file in the upload directory
                    outputUploadFile.open(uploadFilename); // Open for the upload copy
                    if (!outputUploadFile.is_open())       // Check if the file was created successfully
                    {
                        safeErrorPrint("Failed to create output file in upload location: " + uploadFilename); // Log the error
                    }
                    else
                    {
                        safePrint("Created output file in upload location: " + uploadFilename); // Log successful file creation
                    }

                    // Adjust fileData to remove the filename from it
                    fileData = fileData.substr(pos + 1);
                }
                else
                {
                    safeErrorPrint("Error: Filename not found in the first DATA message."); // Log an error if filename extraction fails
                    return;                                                                 // Exit if filename is not found
                }
            }

            // Check if the output file is open and if there is data to write
            if (outputDownloadFile.is_open() && !fileData.empty())
            {
                // Creating a deep copy of fileData for upload
                std::string fileData2 = fileData; // Copy the data for uploading
                // Write the chunk to the download file
                outputDownloadFile.write(fileData.c_str(), fileData.size());                                                 // Write data to the output file
                outputDownloadFile.flush();                                                                                  // Flush the stream to ensure data is written
                safePrint("Written " + std::to_string(fileData.size()) + " bytes to download location: " + currentFilename); // Log the written bytes

                // If the upload file is open, write the same data to it
                if (outputUploadFile.is_open() && !fileData2.empty())
                {
                    outputUploadFile.write(fileData2.c_str(), fileData2.size());                             // Write data to the upload file
                    outputUploadFile.flush();                                                                // Flush the stream
                    safePrint("Written " + std::to_string(fileData2.size()) + " bytes to upload location."); // Log the upload
                    outputUploadFile.close();                                                                // Close the upload file after writing
                }
                else
                {
                    safeErrorPrint("Failed to write to upload location."); // Log an error if the upload fails
                }
            }
            else
            {
                safeErrorPrint("Error: Output file is not open or file data is empty."); // Log an error if output file is not open
            }
        }
        break;
    }
    case MessageType::COMPLETE: // Handling file transfer completion messages
    {
        std::lock_guard<std::mutex> lock(fileMutex);          // Lock for thread-safe file operations
        std::string filename = message.substr(9);             // Extract the filename from the message
        safePrint("File transfer complete for: " + filename); // Log the completion

        // Close the output file after transfer completes
        if (outputDownloadFile.is_open())
        {
            outputDownloadFile.close(); // Close the download file
            currentFilename.clear();    // Clear the current filename for the next file transfer
        }
        break;
    }
    case MessageType::ERROR_: // Handling error messages
    {
        std::string errorDescription = message.substr(6);       // Extract error description
        safeErrorPrint("Error from peer: " + errorDescription); // Log the error
        break;
    }
    case MessageType::STATUS: // Handling status messages
    {
        std::string status = message.substr(7); // Extract status information
        safePrint("Peer status: " + status);    // Log the peer status
        break;
    }
    case MessageType::UNKNOWN: // Handling unknown message types
    default:
    {
        safeErrorPrint("Unknown message type received."); // Log an error for unknown message types
        break;
    }
    }
}

// Display the list of currently connected peers
void Peer::showConnectedPeers()
{
    // Lock the mutex to ensure thread-safe access to the connectedPeers list
    std::lock_guard<std::mutex> lock(peerMutex);

    // Check if there are no connected peers
    if (connectedPeers.empty())
    {
        safePrint("No peers connected."); // Inform the user that there are no connected peers
        return;                           // Exit the function if there are no connected peers
    }

    safePrint("Listing all connected peers:"); // Print header for the list of connected peers

    // Iterate over each connected peer and print their details
    for (const auto &peer : connectedPeers)
    {
        // Construct a string containing the peer's IP address, port, and socket number
        std::string peerDetails = "IP: " + peer.ip +
                                  ", Connected Port: " + std::to_string(peer.port) +
                                  ", Socket: " + std::to_string(peer.socket);

        safePrint(peerDetails); // Print the constructed details of the peer
    }
}

// Display the list of files and their corresponding peers
void Peer::showfilePeers()
{
    // Lock the mutex to ensure thread-safe access to the filePeers map
    std::lock_guard<std::mutex> lock(peerMutex);

    // Check if there are no file peers available
    if (filePeers.empty())
    {
        safePrint("No files available."); // Inform the user that there are no file peers
        return;                           // Exit the function if there are no file peers
    }

    safePrint("Listing all files and their peers:"); // Print header for the list of files and their peers

    // Iterate over each file in the filePeers map
    for (const auto &file : filePeers)
    {
        // Start constructing the details string for the current file
        std::string fileDetails = "File: " + file.first + ", Peers: ";

        // Iterate over each peer associated with the current file
        for (const auto &peer : file.second)
        {
            // Append the peer's information (IP and port) to the file details string
            fileDetails += peer.first + ":" + std::to_string(peer.second) + ", ";
        }

        safePrint(fileDetails); // Print the constructed details of the current file and its associated peers
    }
}

// Display information about the current peer
void Peer::showMyInfo()
{
    // Print the peer's IP address
    safePrint("My IP: " + peerIP);
    // Print the peer's port number
    safePrint("My Port: " + std::to_string(peerPort));
    // Print the peer's socket number
    safePrint("My Socket: " + std::to_string(peerSocket));
    // Print the peer's unique identifier (ID)
    safePrint("My ID: " + peerId);

    // Check if there are any files in the upload path
    // If the upload path is empty, classify the peer as a leecher; otherwise, classify as a seeder
    if (fs::directory_iterator(uploadPath) == fs::directory_iterator())
    {
        safePrint("Current status: Leecher."); // Print leecher status if no files are found
    }
    else
    {
        safePrint("Current status: Seeder."); // Print seeder status if files are present
    }

    // Print the upload directory path of the peer
    safePrint("My Upload Directory: " + uploadPath);
    // Print the download directory path of the peer
    safePrint("My Download Directory: " + downloadPath);
}

int main()
{
    // Configuration for the tracker
    safePrint("Enter Tracker IP: ");
    std::string trackerIP;             // Variable to hold the tracker's IP address
    std::getline(std::cin, trackerIP); // Read the IP address from the console input

    safePrint("Enter Tracker Port: ");
    int trackerPort;         // Variable to hold the tracker's port number
    std::cin >> trackerPort; // Read the port number from the console input

    std::cin.ignore(); // Ignore the newline character left in the input buffer

    // Prompt user for peer's IP address
    safePrint("Enter Peer IP: ");
    std::string peerIP;             // Variable to hold the peer's IP address
    std::getline(std::cin, peerIP); // Read the IP address from the console input

    // Prompt user for peer's port number
    safePrint("Enter Peer Port: ");
    int peerPort;         // Variable to hold the peer's port number
    std::cin >> peerPort; // Read the port number from the console input

    std::cin.ignore(); // Ignore the newline character left in the input buffer

    // Prompt user for peer's ID
    safePrint("Enter Peer ID: ");
    std::string peerId;             // Variable to hold the peer's ID
    std::getline(std::cin, peerId); // Read the ID from the console input

    // Prompt user for upload directory
    safePrint("Enter Upload Directory: ");
    std::string uploadDir;             // Variable to hold the upload directory path
    std::getline(std::cin, uploadDir); // Read the upload directory path from the console input

    // Prompt user for download directory
    safePrint("Enter Download Directory: ");
    std::string downloadDir;             // Variable to hold the download directory path
    std::getline(std::cin, downloadDir); // Read the download directory path from the console input

    // Create a Peer instance with the provided configurations
    Peer peer(peerIP, peerPort, peerId, trackerIP, trackerPort, uploadDir, downloadDir);

    // Inform the user that the peer is starting
    safePrint("Starting peer on " + peerIP + ":" + std::to_string(peerPort) + "...");

    // Start the peer's operations in a separate thread
    std::thread peerThread(&Peer::start, &peer); // Launch the peer's thread for background processing

    // Attempt to connect to the tracker
    if (!peer.connectToTracker())
    {
        safePrint("Failed to connect to tracker!"); // Inform user of failure
        return 1;                                   // Exit the program with an error code
    }

    int choice; // Variable to store user's menu choice

    // Infinite loop to display the peer menu and handle user choices
    while (true)
    {
        // Display the menu options
        safePrint("\n=== Peer Menu ===\n"
                  "1. Announce to Tracker\n"
                  "2. Request Peers\n"
                  "3. Connect to Peer\n"
                  "4. Request File from Peer\n"
                  "5. Disconnect from Peer\n"
                  "6. Disconnect All Peers\n"
                  "7. Show Connected Peers\n"
                  "8. Show File Peers\n"
                  "9. Show My Info\n"
                  "10. Exit\n"
                  "Choose an option: ");

        std::cin >> choice; // Read user's choice

        // Handle user's menu choice using a switch statement
        switch (choice)
        {
        case 1: // Announce to tracker
        {
            // Attempt to announce the peer's presence to the tracker
            if (peer.announceToTracker())
            {
                safePrint("Successfully announced to tracker."); // Success message
            }
            else
            {
                safeErrorPrint("Failed to announce to tracker."); // Error message
            }
            break; // Exit the case
        }
        case 2: // Request peers for a specific file
        {
            std::string filename; // Variable to hold the requested filename
            safePrint("Enter filename to request peers for: ");
            std::cin >> filename; // Read the filename from the console

            // Request peers sharing the specified file from the tracker
            peer.requestPeers(filename);
            break; // Exit the case
        }
        case 3: // Connect to another peer
        {
            std::string incomingPeerIP; // Variable to hold the incoming peer's IP address
            int incomingPeerPort;       // Variable to hold the incoming peer's port number
            safePrint("Enter peer IP: ");
            std::cin >> incomingPeerIP; // Read the incoming peer's IP address
            safePrint("Enter peer port: ");
            std::cin >> incomingPeerPort; // Read the incoming peer's port number

            // Attempt to connect to the specified peer
            if (peer.connectToPeer(incomingPeerIP, incomingPeerPort))
            {
                safePrint("Connected to peer " + incomingPeerIP + ":" + std::to_string(incomingPeerPort)); // Success message
            }
            else
            {
                safeErrorPrint("Failed to connect to peer."); // Error message
            }
            break; // Exit the case
        }
        case 4: // Request a file from a peer
        {
            std::string filename; // Variable to hold the requested filename
            safePrint("Enter filename to request from peer: ");
            std::cin >> filename; // Read the filename from the console

            // Check if the requested file has peers available
            if (peer.filePeers.find(filename) == peer.filePeers.end())
            {
                safePrint("Requesting peers for the file..."); // Inform user of request
                peer.requestPeers(filename);                   // Request peers for the file from the tracker
            }

            // Ensure we have connected peers to request the file from
            if (!peer.connectedPeers.empty())
            {
                // Extract the first peer's IP and port from the filePeers map
                std::string peerIP;
                int peerPort;
                for (const auto &peerInfo : peer.filePeers[filename])
                {
                    peerIP = peerInfo.first;    // Get the IP address of the peer
                    peerPort = peerInfo.second; // Get the port number of the peer
                    break;                      // Break after first peer
                }

                // Find the socket associated with the selected peer from connectedPeers
                SOCKET peerSocket; // Variable to hold the peer's socket
                for (const auto &peerInfo : peer.connectedPeers)
                {
                    if (peerInfo.ip == peerIP && peerInfo.port == peerPort) // Match found
                    {
                        peerSocket = peerInfo.socket; // Assign the socket
                        break;                        // Break after finding the socket
                    }
                }

                // Request the file from the specified peer
                peer.requestFile(filename, peerSocket);
            }
            else
            {
                safeErrorPrint("No connected peers available to request the file from."); // Error message
            }
            break; // Exit the case
        }
        case 5: // Disconnect from a specific peer
        {
            std::string peerIP; // Variable to hold the peer's IP address to disconnect
            int peerPort;       // Variable to hold the peer's port number to disconnect
            safePrint("Enter peer IP to disconnect: ");
            std::cin >> peerIP; // Read the IP address of the peer to disconnect
            safePrint("Enter peer port to disconnect: ");
            std::cin >> peerPort; // Read the port number of the peer to disconnect

            // Check if the specified peer is connected
            if (peer.isPeerConnected(peerIP, peerPort))
            {
                // Iterate through connected peers to find and disconnect
                for (auto &peerInfo : peer.connectedPeers)
                {
                    if (peerInfo.ip == peerIP && peerInfo.port == peerPort) // Match found
                    {
                        peer.disconnectPeer(peerInfo.socket);                                            // Disconnect the peer
                        safePrint("Disconnected from peer: " + peerIP + ":" + std::to_string(peerPort)); // Success message

                        // Remove the disconnected peer from the connectedPeers list
                        peer.connectedPeers.erase(std::remove_if(peer.connectedPeers.begin(), peer.connectedPeers.end(),
                                                                 [&](const Peer::PeerInfo &peer)
                                                                 {
                                                                     return peer.ip == peerIP && peer.port == peerPort; // Condition for removal
                                                                 }),
                                                  peer.connectedPeers.end()); // Finalize the removal
                        break;                                                // Exit the loop after disconnecting
                    }
                }
            }
            else
            {
                safeErrorPrint("Peer not connected."); // Error message
            }
            break; // Exit the case
        }
        case 6: // Disconnect from all peers
        {
            peer.disconnectAllPeers();                 // Call method to disconnect from all peers
            safePrint("Disconnected from all peers."); // Success message
            break;                                     // Exit the case
        }
        case 7: // Show all connected peers
        {
            // Call method to display all connected peers
            peer.showConnectedPeers();
            break; // Exit the case
        }
        case 8: // Show files and their peers
        {
            // Call method to display all files and their associated peers
            peer.showfilePeers();
            break; // Exit the case
        }
        case 9: // Show information about the current peer
        {
            // Call method to display the peer's information
            peer.showMyInfo();
            break; // Exit the case
        }
        case 10: // Exit the application
        {
            safePrint("Exiting..."); // Inform the user about exit

            // Set the running flag to false to signal the peer thread to stop
            peer.isRunning = false;

            // Close the listening socket to unblock any pending accept() calls
            closesocket(peer.peerSocket);

            // Ensure the peer thread exits properly
            if (peerThread.joinable())
            {
                peerThread.join(); // Wait for the peer thread to finish
            }

            safePrint("Peer thread has exited."); // Final message
            return 0;                             // Exit the program successfully
        }

        default:                                                 // Handle invalid choices
            safeErrorPrint("Invalid choice. Please try again."); // Error message for invalid choice
        }
    }

    return 0; // Redundant return statement, as it should never reach here
}
