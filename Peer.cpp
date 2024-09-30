#include "Peer.h"

std::mutex consoleMutex;

void safePrint(const std::string &message)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cout << message << std::endl;
}

void safeErrorPrint(const std::string &message)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cerr << message << std::endl;
}

// Peer constructor
Peer::Peer(const std::string &ip, int port, const std::string &id, const std::string &trackerIp, int trackerPort,
           const std::string &uploadDir, const std::string &downloadDir)
    : peerIP(ip), peerPort(port), peerId(id), trackerIP(trackerIp), trackerPort(trackerPort),
      uploadPath(uploadDir), downloadPath(downloadDir), fileSize(0), trackerSocket(INVALID_SOCKET), isRunning(true)
{
    // Initialize Winsock
    WSAData wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        safeErrorPrint("WSAStartup failed!");
        // Handle the error, potentially throwing an exception or marking the object as unusable
    }

    peerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (peerSocket == INVALID_SOCKET)
    {
        safeErrorPrint("Socket creation failed!");
        // Handle the error, potentially throwing an exception or marking the object as unusable
    }

    sockaddr_in peerAddr;
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_addr.s_addr = inet_addr(peerIP.c_str());
    peerAddr.sin_port = htons(peerPort);
    if (bind(peerSocket, (sockaddr *)&peerAddr, sizeof(peerAddr)) == SOCKET_ERROR)
    {
        safeErrorPrint("Bind failed!");
        // Handle the error, potentially throwing an exception or marking the object as unusable
    }

    if (listen(peerSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        safeErrorPrint("Listen failed!");
        // Handle the error, potentially throwing an exception or marking the object as unusable
    }
}

// Destructor
Peer::~Peer()
{
    isRunning = false; // Set the running flag to false

    // Close the listening socket to unblock accept()
    if (peerSocket != INVALID_SOCKET)
    {
        closesocket(peerSocket);
        peerSocket = INVALID_SOCKET;
    }

    disconnectAllPeers(); // Ensure all peer connections are closed

    // Join all peer handling threads
    for (std::thread &t : otherPeerThreads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    WSACleanup(); // Clean up Winsock
}

// Disconnect all peers safely
void Peer::disconnectAllPeers()
{
    std::lock_guard<std::mutex> lock(mtx); // Ensure thread-safe access to connectedPeers
    for (auto &peer : connectedPeers)
    {
        closesocket(peer.socket); // Close each peer's socket
    }
    connectedPeers.clear(); // Clear the vector of connected peers
}

// Start the peer
void Peer::start()
{
    safePrint("Peer started on " + peerIP + ":" + std::to_string(peerPort));
    while (isRunning)
    {
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientFd = accept(peerSocket, (sockaddr *)&clientAddr, &clientAddrLen);
        if (clientFd == INVALID_SOCKET)
        {
            if (!isRunning)
                break; // Exit the loop when shutting down
            safeErrorPrint("Accept failed!");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mtx); // Lock access to otherPeerThreads
            // if not present in otherPeerThreads
            if (std::find_if(otherPeerThreads.begin(), otherPeerThreads.end(),
                             [clientFd](std::thread &t)
                             { return t.native_handle() == clientFd; }) == otherPeerThreads.end())
            {
                otherPeerThreads.emplace_back(&Peer::handlePeer, this, clientFd, clientAddr);
            }
        }
    }

    safePrint("Peer is no longer accepting connections.");
}

// Method to handle a peer
void Peer::handlePeer(SOCKET clientFd, sockaddr_in clientAddr)
{
    char ipBuffer[INET_ADDRSTRLEN];                                       // Buffer for IP address
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuffer, sizeof(ipBuffer)); // Convert IP to string
    int clientPort = ntohs(clientAddr.sin_port);                          // Convert port to host byte order

    safePrint("Peer connected: " + std::string(ipBuffer) + ":" + std::to_string(clientPort));
    {
        std::lock_guard<std::mutex> lock(peerMutex); // Lock before adding to connectedPeers
        // If not present in connectedPeers
        if (std::find_if(connectedPeers.begin(), connectedPeers.end(),
                         [clientFd](const PeerInfo &peer)
                         { return peer.socket == clientFd; }) == connectedPeers.end())
        {
            connectedPeers.emplace_back(PeerInfo{std::string(ipBuffer), clientPort, clientFd});
        }
    }
    while (true)
    {
        char buffer[4096];
        int bytesReceived;

        bytesReceived = recv(clientFd, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0'; // Null-terminate the buffer
            std::string message(buffer);
            safePrint("Received message from peer: " + message);

            // Process message safely
            processMessage(message, clientFd);
        }
        else
        {
            safeErrorPrint("Connection lost with peer: " + std::string(ipBuffer) + ":" + std::to_string(clientPort));
            break; // Exit the loop if the connection is lost
        }
    }

    {
        std::lock_guard<std::mutex> lock(peerMutex); // Lock before closing the peer socket
        closesocket(clientFd);                       // Close the client socket safely
    }
    safePrint("Peer disconnected: " + std::string(ipBuffer) + ":" + std::to_string(clientPort));
    // Removing the disconnected peer from the connectedPeers list
    connectedPeers.erase(std::remove_if(connectedPeers.begin(), connectedPeers.end(),
                                        [clientFd](const PeerInfo &peer)
                                        { return peer.socket == clientFd; }),
                         connectedPeers.end());
}

// Method to connect to the tracker
bool Peer::connectToTracker()
{
    // Create a socket for the tracker connection
    trackerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (trackerSocket == INVALID_SOCKET)
    {
        safeErrorPrint("Socket creation for tracker failed!");
        return false;
    }

    // Set up the tracker address structure
    sockaddr_in trackerAddr;
    memset(&trackerAddr, 0, sizeof(trackerAddr));
    trackerAddr.sin_family = AF_INET;
    trackerAddr.sin_port = htons(trackerPort);                  // Use member variable trackerPort
    trackerAddr.sin_addr.s_addr = inet_addr(trackerIP.c_str()); // Use member variable trackerIP

    if (connect(trackerSocket, (sockaddr *)&trackerAddr, sizeof(trackerAddr)) == SOCKET_ERROR)
    {
        safeErrorPrint("Failed to connect to tracker at: " + trackerIP + ":" + std::to_string(trackerPort));
        closesocket(trackerSocket);
        trackerSocket = INVALID_SOCKET; // Reset socket state
        return false;
    }
    safePrint("Successfully connected to tracker at: " + trackerIP + ":" + std::to_string(trackerPort));
    return true;
}

// Method to announce to the tracker
bool Peer::announceToTracker()
{
    // Ensure the upload path is valid
    if (!fs::exists(uploadPath) || !fs::is_directory(uploadPath))
    {
        safeErrorPrint("Upload path does not exist or is not a directory!");
        return false;
    }

    std::lock_guard<std::mutex> fileLock(fileMutex); // Lock file operations

    // Iterate through all files in the upload directory
    for (const auto &entry : fs::directory_iterator(uploadPath))
    {
        if (entry.is_regular_file())
        {
            // Get the file name from the entry
            std::string fileName = entry.path().filename().string();

            {
                std::lock_guard<std::mutex> lock(peerMutex); // Lock access to filePeers
                // If not present in filePeers[filename]
                if (std::find_if(filePeers[fileName].begin(), filePeers[fileName].end(),
                                 [peerPort = peerPort](const std::pair<std::string, int> &peer)
                                 { return peer.second == peerPort; }) == filePeers[fileName].end())
                {
                    filePeers[fileName].emplace_back(peerIP, peerPort); // Add the peer to the filePeers map
                }
            }

            // Construct the announce message for the file
            std::string announceMessage = "announce " + fileName + ":" + std::to_string(peerPort);

            // Synchronize sending the announce message
            int bytesSent;
            bytesSent = send(trackerSocket, announceMessage.c_str(), announceMessage.size(), 0);

            if (bytesSent == SOCKET_ERROR)
            {
                safeErrorPrint("Failed to send announce message to tracker for file: " + fileName);
                return false;
            }

            safePrint("Announced to tracker: " + announceMessage);

            // Receive the response from the tracker
            char buffer[4096];
            int bytesReceived;
            bytesReceived = recv(trackerSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived > 0)
            {
                buffer[bytesReceived] = '\0'; // Null-terminate the response
                std::string response(buffer);
                safePrint("Tracker response for file " + fileName + ": " + response);
            }
            else
            {
                safeErrorPrint("Failed to receive response from tracker for file: " + fileName);
                return false;
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

    send(trackerSocket, requestMessage.c_str(), requestMessage.size(), 0);

    safePrint("Requested peers list for file '" + requiredFile + "' from tracker.");

    char buffer[4096];
    int bytesReceived;

    bytesReceived = recv(trackerSocket, buffer, sizeof(buffer) - 1, 0);

    if (bytesReceived > 0)
    {
        buffer[bytesReceived] = '\0'; // Null-terminate the received data
        std::string response(buffer);

        // Check if the response contains "No peers available"
        if (response.find("No peers available for this file") != std::string::npos)
        {
            safePrint("Tracker response: " + response);
            return; // No peers are available, so we exit the function
        }

        safePrint("Received peers list: " + response);

        // Split the response by newline to get individual peer IP:port pairs
        std::stringstream ss(response);
        std::string peerEntry;

        // Connect to each peer in the list
        while (std::getline(ss, peerEntry, '\n'))
        {
            size_t colonPos = peerEntry.find(":");
            if (colonPos != std::string::npos)
            {
                std::string peerIP = peerEntry.substr(0, colonPos);
                int peerPort = std::stoi(peerEntry.substr(colonPos + 1));

                // Add the peer to the filePeers map
                {
                    std::lock_guard<std::mutex> lock(mtx); // Lock filePeers access
                    // If not present in filePeers[requiredFile]
                    if (std::find_if(filePeers[requiredFile].begin(), filePeers[requiredFile].end(),
                                     [peerPort](const std::pair<std::string, int> &peer)
                                     { return peer.second == peerPort; }) == filePeers[requiredFile].end())
                    {
                        filePeers[requiredFile].emplace_back(peerIP, peerPort);
                    }
                }

                // Attempt to connect to the peer
                connectToPeer(peerIP, peerPort);
            }
        }
    }
    else
    {
        safeErrorPrint("Failed to receive peers list from tracker!");
    }
}

bool Peer::connectToPeer(const std::string &incomingPeerIP, int incomingPeerPort)
{
    safePrint("Connecting to peer: " + incomingPeerIP + ":" + std::to_string(incomingPeerPort));
    // Check if peer is already connected
    {
        std::lock_guard<std::mutex> lock(mtx); // Lock connectedPeers for reading
        for (const auto &peerInfo : connectedPeers)
        {
            if (peerInfo.ip == incomingPeerIP && peerInfo.port == incomingPeerPort && peerInfo.socket != INVALID_SOCKET)
            {
                safeErrorPrint("Peer " + incomingPeerIP + ":" + std::to_string(incomingPeerPort) + " is already connected.");
                return true; // Peer already connected, no need to connect again
            }
        }
    }
    // Create a socket for the incoming peer connection
    SOCKET incomingPeerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (incomingPeerSocket == INVALID_SOCKET)
    {
        safeErrorPrint("Socket creation failed!");
        return false;
    }

    sockaddr_in peerAddr;
    memset(&peerAddr, 0, sizeof(peerAddr));
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(incomingPeerPort);
    peerAddr.sin_addr.s_addr = inet_addr(incomingPeerIP.c_str());

    // Check for valid IP address
    if (peerAddr.sin_addr.s_addr == INADDR_NONE)
    {
        safeErrorPrint("Invalid peer IP address: " + incomingPeerIP);
        closesocket(incomingPeerSocket);
        return false;
    }
    // Attempt to connect to the peer
    if (connect(incomingPeerSocket, (sockaddr *)&peerAddr, sizeof(peerAddr)) == SOCKET_ERROR)
    {
        int errorCode = WSAGetLastError(); // For Windows
        safeErrorPrint("Failed to connect to peer: " + incomingPeerIP + ":" + std::to_string(incomingPeerPort) +
                       ". Error code: " + std::to_string(errorCode));
        closesocket(incomingPeerSocket);
        return false;
    }

    // Store the connected peer's socket in the connectedPeers list
    {
        std::lock_guard<std::mutex> lock(mtx); // Lock connectedPeers for modification
        // If not present in connectedPeers
        if (std::find_if(connectedPeers.begin(), connectedPeers.end(),
                         [incomingPeerIP, incomingPeerPort](const PeerInfo &peer)
                         { return peer.ip == incomingPeerIP && peer.port == incomingPeerPort; }) == connectedPeers.end())
        {
            connectedPeers.emplace_back(PeerInfo{incomingPeerIP, incomingPeerPort, incomingPeerSocket});
        }
    }

    // Send handshake with listening port
    sendHandshake(incomingPeerSocket, peerPort, peerSocket);
    // Start a listener thread for incoming messages from the peer
    std::thread listenerThread(&Peer::listenForMessages, this, incomingPeerSocket);
    listenerThread.detach(); // Detach the thread, so it runs independently

    return true;
}

void Peer::sendMessage(const std::string &message, SOCKET connectedPeerSocket)
{
    // Send the message to the connected peer socket
    int result = send(connectedPeerSocket, message.c_str(), message.size(), 0);
    if (result == SOCKET_ERROR)
    {
        int errorCode = WSAGetLastError(); // For Windows systems
        safeErrorPrint("Failed to send message to peer socket: " + std::to_string(connectedPeerSocket) +
                       "! Error code: " + std::to_string(errorCode));
    }
    else
    {
        safePrint("Message sent to peer socket: " + std::to_string(connectedPeerSocket) +
                  " with message: " + message);
    }
}

// Receive a message from the peer
std::string Peer::receiveMessage(SOCKET peerSocket)
{
    char buffer[2048];
    int bytesReceived = recv(peerSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0)
    {
        buffer[bytesReceived] = '\0'; // Null-terminate
        return std::string(buffer);
    }
    else
    {
        safeErrorPrint("Connection lost with a peer!");
        return "";
    }
}

// Listen for incoming messages from a specific peer
void Peer::listenForMessages(SOCKET peerSocket)
{
    char buffer[4096];
    while (true) // Continuously listen for messages
    {
        int bytesReceived = recv(peerSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0'; // Null-terminate the received message
            std::string message(buffer);
            safePrint("Received message: " + message);

            // Process the received message
            processMessage(message, peerSocket);
        }
        else
        {
            safeErrorPrint("Connection lost with a peer!");
            break; // Exit the loop if the connection is lost
        }
    }

    // Cleanup after the peer has disconnected
    disconnectPeer(peerSocket);
}

// Method to disconnect a specific peer using its socket
void Peer::disconnectPeer(SOCKET peerSocket)
{
    {
        std::lock_guard<std::mutex> lock(mtx); // Lock for protecting connectedPeers
        closesocket(peerSocket);

        // Remove the peer from the connectedPeers list
        connectedPeers.erase(std::remove_if(connectedPeers.begin(), connectedPeers.end(),
                                            [peerSocket](const PeerInfo &peer)
                                            { return peer.socket == peerSocket; }),
                             connectedPeers.end());
    }

    safePrint("Disconnected peer with socket: " + std::to_string(peerSocket));
}

// Function to request a file from a peer
void Peer::requestFile(const std::string &filename, SOCKET peerSocket)
{
    std::string requestMessage = "REQUEST " + filename; // Construct the request message
    sendMessage(requestMessage, peerSocket);            // Send the request to the peer
    safePrint("Requested file: " + filename);
}

// Function to acknowledge a request
void Peer::sendAck(const std::string &filename, SOCKET peerSocket)
{
    std::string ackMessage = "ACK " + filename; // Construct ACK message
    sendMessage(ackMessage, peerSocket);        // Send ACK
    safePrint("Sent ACK for: " + filename);
}

// Check if a specific peer is connected
bool Peer::isPeerConnected(const std::string &peerIP, int peerPort)
{
    std::lock_guard<std::mutex> lock(mtx); // Lock for protecting connectedPeers

    // Check if the peer is in the connectedPeers list
    auto it = std::find_if(connectedPeers.begin(), connectedPeers.end(),
                           [&](const PeerInfo &peer)
                           {
                               return peer.ip == peerIP && peer.port == peerPort;
                           });

    // Return true if the peer is found, false otherwise
    return it != connectedPeers.end();
}

// Send handshake with listening port after connection is established
void Peer::sendHandshake(SOCKET connectedPeerSocket, int listeningPort, SOCKET listeningSocket)
{
    // std::lock_guard<std::mutex> lock(socketMutex); // Lock for socket communication
    sendMessage("HANDSHAKE " + std::to_string(listeningPort), connectedPeerSocket);
}

// Determine the message type
MessageType Peer::getMessageType(const std::string &message)
{
    if (message.find("REQUEST") == 0)
        return MessageType::REQUEST;
    if (message.find("ACK") == 0)
        return MessageType::ACK;
    if (message.find("DATA") == 0)
        return MessageType::DATA;
    if (message.find("COMPLETE") == 0)
        return MessageType::COMPLETE;
    if (message.find("ERROR") == 0)
        return MessageType::ERROR_;
    if (message.find("STATUS") == 0)
        return MessageType::STATUS;
    if (message.find("HANDSHAKE") == 0)
        return MessageType::HANDSHAKE;
    return MessageType::UNKNOWN;
}

// Process incoming messages
void Peer::processMessage(const std::string &message, SOCKET connectedPeerSocket)
{
    // Extract connected peer's IP and port
    sockaddr_in peerAddr;
    int addrLen = sizeof(peerAddr);
    if (getpeername(connectedPeerSocket, (sockaddr *)&peerAddr, &addrLen) == SOCKET_ERROR)
    {
        safeErrorPrint("Failed to get peer name!");
        return;
    }

    char ipBuffer[INET_ADDRSTRLEN];                                     // Buffer for IP address
    inet_ntop(AF_INET, &peerAddr.sin_addr, ipBuffer, sizeof(ipBuffer)); // Convert IP to string
    int connectedPeerPort = ntohs(peerAddr.sin_port);                   // Convert port to host byte order

    safePrint("Received message from " + std::string(ipBuffer) + ":" + std::to_string(connectedPeerPort) + " - " + message);

    MessageType type = getMessageType(message);

    switch (type)
    {
    case MessageType::REQUEST:
    {
        std::lock_guard<std::mutex> lock(fileMutex);        // Lock for file operations
        std::string filename = message.substr(8);           // Extract filename
        std::string fullPath = uploadPath + "/" + filename; // Construct full path for upload

        // Send ACK for the request
        sendMessage("ACK " + filename, connectedPeerSocket);
        std::ifstream file(fullPath, std::ios::binary); // Open file from upload path
        if (file.is_open())
        {
            char fileBuffer[1024];
            while (file.read(fileBuffer, sizeof(fileBuffer)))
            {
                std::string dataChunk(fileBuffer, file.gcount());                       // Lock for socket communication
                sendMessage("DATA " + filename + " " + dataChunk, connectedPeerSocket); // Send data chunk
            }
            if (file.gcount() > 0)
            {
                std::string dataChunk(fileBuffer, file.gcount());
                sendMessage("DATA " + filename + " " + dataChunk, connectedPeerSocket); // Send remaining data
            }
            // Wait for a short time before sending the completion message
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sendMessage("COMPLETE " + filename, connectedPeerSocket); // Notify completion

            file.close();
            safePrint("File transfer complete: " + filename);
        }
        else
        {                                                                          // Lock for socket communication
            sendMessage("ERROR File not found: " + filename, connectedPeerSocket); // Send error message
            safeErrorPrint("Failed to open file: " + fullPath);
        }

        break;
    }
    case MessageType::HANDSHAKE:
    {
        std::string port = message.substr(10); // Extract the port from the message
        int listeningPort = std::stoi(port);
        safePrint("Received handshake with listening port: " + std::to_string(listeningPort));

        // Update the peer info with the received listening port
        {
            std::lock_guard<std::mutex> lock(peerMutex); // Lock the peer list
            for (auto &peer : connectedPeers)
            {
                if (peer.socket == connectedPeerSocket)
                {
                    peer.port = listeningPort;
                    safePrint("Updated peer " + std::string(ipBuffer) + " with listening port: " + std::to_string(listeningPort));
                    break;
                }
            }
        }
        break;
    }
    case MessageType::ACK:
    {
        std::string filename = message.substr(4);
        safePrint("Received ACK for file: " + filename);
        break;
    }
    case MessageType::DATA:
    {
        std::string fileData = message.substr(5); // Extract the file data from the message

        {
            std::lock_guard<std::mutex> lock(fileMutex); // Lock for file operations

            if (currentFilename.empty())
            {
                // This is the first DATA message; we need to extract the filename
                size_t pos = fileData.find(" ");
                if (pos != std::string::npos)
                {
                    std::string filename = fileData.substr(0, pos);
                    currentFilename = downloadPath + "/" + filename;          // Use downloadPath for output file
                    std::string uploadFilename = uploadPath + "/" + filename; // Use uploadPath for another copy

                    // Open the output file in the download directory
                    outputDownloadFile.open(currentFilename);
                    if (!outputDownloadFile.is_open())
                    {
                        safeErrorPrint("Failed to create output file: " + currentFilename);
                        return; // Exit if file creation fails
                    }
                    safePrint("Created output file in download location: " + currentFilename);

                    outputUploadFile.open(uploadFilename); // Open the output file in the upload directory
                    if (!outputUploadFile.is_open())
                    {
                        safeErrorPrint("Failed to create output file in upload location: " + uploadFilename);
                    }
                    else
                    {
                        safePrint("Created output file in upload location: " + uploadFilename);
                    }

                    // Adjust fileData to remove the filename from it
                    fileData = fileData.substr(pos + 1);
                }
                else
                {
                    safeErrorPrint("Error: Filename not found in the first DATA message.");
                    return;
                }
            }

            // Check if file is open and there is file data to write
            if (outputDownloadFile.is_open() && !fileData.empty())
            {
                // Creating a deep copy of fileData
                std::string fileData2 = fileData;
                // Write the chunk to the download file
                outputDownloadFile.write(fileData.c_str(), fileData.size());
                outputDownloadFile.flush();
                safePrint("Written " + std::to_string(fileData.size()) + " bytes to download location: " + currentFilename);

                if (outputUploadFile.is_open() && !fileData2.empty())
                {
                    outputUploadFile.write(fileData2.c_str(), fileData2.size());
                    outputUploadFile.flush();
                    safePrint("Written " + std::to_string(fileData2.size()) + " bytes to upload location.");
                    outputUploadFile.close(); // Close the upload file after writing
                }
                else
                {
                    safeErrorPrint("Failed to write to upload location.");
                }
            }
            else
            {
                safeErrorPrint("Error: Output file is not open or file data is empty.");
            }
        }

        break;
    }
    case MessageType::COMPLETE:
    {
        std::lock_guard<std::mutex> lock(fileMutex); // Lock for file operations
        std::string filename = message.substr(9);
        safePrint("File transfer complete for: " + filename);

        // Close the file after transfer completes
        if (outputDownloadFile.is_open())
        {
            outputDownloadFile.close();
            currentFilename.clear(); // Clear current filename for next file transfer
        }
        break;
    }
    case MessageType::ERROR_:
    {
        std::string errorDescription = message.substr(6);
        safeErrorPrint("Error from peer: " + errorDescription);
        break;
    }
    case MessageType::STATUS:
    {
        std::string status = message.substr(7);
        safePrint("Peer status: " + status);
        break;
    }
    case MessageType::UNKNOWN:
    default:
    {
        safeErrorPrint("Unknown message type received.");
        break;
    }
    }
}

void Peer::showConnectedPeers()
{
    std::lock_guard<std::mutex> lock(peerMutex);

    if (connectedPeers.empty())
    {
        safePrint("No peers connected.");
        return;
    }

    safePrint("Listing all connected peers:");

    for (const auto &peer : connectedPeers)
    {
        std::string peerDetails = "IP: " + peer.ip +
                                  ", Connected Port: " + std::to_string(peer.port) +
                                  ", Socket: " + std::to_string(peer.socket);

        safePrint(peerDetails);
    }
}

void Peer::showfilePeers()
{
    std::lock_guard<std::mutex> lock(peerMutex);

    if (filePeers.empty())
    {
        safePrint("No files available.");
        return;
    }

    safePrint("Listing all files and their peers:");

    for (const auto &file : filePeers)
    {
        std::string fileDetails = "File: " + file.first + ", Peers: ";
        for (const auto &peer : file.second)
        {
            fileDetails += peer.first + ":" + std::to_string(peer.second) + ", ";
        }

        safePrint(fileDetails);
    }
}

void Peer::showMyInfo()
{
    safePrint("My IP: " + peerIP);
    safePrint("My Port: " + std::to_string(peerPort));
    safePrint("My Socket: " + std::to_string(peerSocket));
    safePrint("My ID: " + peerId);
    // If no file present in uploadpath then leecher, else seeder
    if (fs::directory_iterator(uploadPath) == fs::directory_iterator())
    {
        safePrint("Current status: Leecher.");
    }
    else
    {
        safePrint("Current status: Seeder.");
    }
    safePrint("My Upload Directory: " + uploadPath);
    safePrint("My Download Directory: " + downloadPath);
}

int main()
{
    // Configuration for Peer and Tracker
    std::string trackerIP = "192.168.172.183"; // Change this to your tracker IP
    int trackerPort = 9999;                    // Change this to your tracker port

    safePrint("Enter Peer IP: ");
    std::string peerIP;             // Peer's IP address
    std::getline(std::cin, peerIP); // Read the peer's IP address from the console

    safePrint("Enter Peer Port: ");
    int peerPort;         // Peer's port number
    std::cin >> peerPort; // Read the peer's port number from the console

    std::cin.ignore(); // Ignore the newline character

    safePrint("Enter Peer ID: ");
    std::string peerId;             // Peer's ID
    std::getline(std::cin, peerId); // Read the peer's ID from the console

    safePrint("Enter Upload Directory: ");
    std::string uploadDir;             // Directory for files to share
    std::getline(std::cin, uploadDir); // Read the upload directory from the console

    safePrint("Enter Download Directory: ");
    std::string downloadDir;             // Directory for downloaded files
    std::getline(std::cin, downloadDir); // Read the download directory from the console

    // Create a Peer instance
    Peer peer(peerIP, peerPort, peerId, trackerIP, trackerPort, uploadDir, downloadDir);

    safePrint("Starting peer on " + peerIP + ":" + std::to_string(peerPort) + "...");
    std::thread peerThread(&Peer::start, &peer); // Start the peer's thread

    // Connect to the tracker
    if (!peer.connectToTracker())
    {
        safePrint("Failed to connect to tracker!");
        return 1;
    }

    int choice;
    while (true)
    {
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

        std::cin >> choice;

        switch (choice)
        {
        case 1:
        {
            if (peer.announceToTracker())
            {
                safePrint("Successfully announced to tracker.");
            }
            else
            {
                safeErrorPrint("Failed to announce to tracker.");
            }
            break;
        }
        case 2:
        {
            std::string filename;
            safePrint("Enter filename to request peers for: ");
            std::cin >> filename;

            // Request peers sharing the file from the tracker
            peer.requestPeers(filename);
            break;
        }
        case 3:
        {
            std::string incomingPeerIP;
            int incomingPeerPort;
            safePrint("Enter peer IP: ");
            std::cin >> incomingPeerIP;
            safePrint("Enter peer port: ");
            std::cin >> incomingPeerPort;

            if (peer.connectToPeer(incomingPeerIP, incomingPeerPort))
            {
                safePrint("Connected to peer " + incomingPeerIP + ":" + std::to_string(incomingPeerPort));
            }
            else
            {
                safeErrorPrint("Failed to connect to peer.");
            }
            break;
        }
        case 4:
        {
            std::string filename;
            safePrint("Enter filename to request from peer: ");
            std::cin >> filename;

            if (peer.filePeers.find(filename) == peer.filePeers.end())
            {
                safePrint("Requesting peers for the file...");
                peer.requestPeers(filename);
            }

            // Ensure we have peers to request the file from
            if (!peer.connectedPeers.empty())
            {
                // Find ip and port from filePeers
                std::string peerIP;
                int peerPort;
                for (const auto &peerInfo : peer.filePeers[filename])
                {
                    peerIP = peerInfo.first;
                    peerPort = peerInfo.second;
                    break;
                }
                // Find socket from connectedPeers
                SOCKET peerSocket;
                for (const auto &peerInfo : peer.connectedPeers)
                {
                    if (peerInfo.ip == peerIP && peerInfo.port == peerPort)
                    {
                        peerSocket = peerInfo.socket;
                        break;
                    }
                }
                peer.requestFile(filename, peerSocket);
            }
            else
            {
                safeErrorPrint("No connected peers available to request the file from.");
            }
            break;
        }
        case 5:
        {
            std::string peerIP;
            int peerPort;
            safePrint("Enter peer IP to disconnect: ");
            std::cin >> peerIP;
            safePrint("Enter peer port to disconnect: ");
            std::cin >> peerPort;

            if (peer.isPeerConnected(peerIP, peerPort))
            {
                for (auto &peerInfo : peer.connectedPeers)
                {
                    if (peerInfo.ip == peerIP && peerInfo.port == peerPort)
                    {
                        peer.disconnectPeer(peerInfo.socket);
                        safePrint("Disconnected from peer: " + peerIP + ":" + std::to_string(peerPort));
                        peer.connectedPeers.erase(std::remove_if(peer.connectedPeers.begin(), peer.connectedPeers.end(),
                                                                 [&](const Peer::PeerInfo &peer)
                                                                 {
                                                                     return peer.ip == peerIP && peer.port == peerPort;
                                                                 }),
                                                  peer.connectedPeers.end());
                        break;
                    }
                }
            }
            else
            {
                safeErrorPrint("Peer not connected.");
            }
            break;
        }
        case 6:
        {
            peer.disconnectAllPeers();
            safePrint("Disconnected from all peers.");
            break;
        }
        case 7:
        {
            // Display all connected peers
            peer.showConnectedPeers();
            break;
        }
        case 8:
        {
            // Display all files and their peers
            peer.showfilePeers();
            break;
        }
        case 9:
        {
            // Display peer's information
            peer.showMyInfo();
            break;
        }
        case 10:
        {
            safePrint("Exiting...");

            // Set isRunning to false to signal the peer thread to stop
            peer.isRunning = false;

            // Close the listening socket to unblock accept()
            closesocket(peer.peerSocket);

            // Ensure the peer thread exits properly
            if (peerThread.joinable())
            {
                peerThread.join();
            }

            safePrint("Peer thread has exited.");
            return 0;
        }

        default:
            safeErrorPrint("Invalid choice. Please try again.");
        }
    }

    return 0;
}