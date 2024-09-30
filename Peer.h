#ifndef PEER_H
#define PEER_H

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <map>
#include <winsock2.h> // For SOCKET
#include <ws2tcpip.h> // For sockaddr_in
#include <vector>
#include <cstddef> // For std::size_t
#include <algorithm>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <atomic>

namespace fs = std::filesystem;

/*
 *  Enum class for MessageType
 *  @brief: Defines the types of messages that can be exchanged between peers.
 */
enum class MessageType
{
    REQUEST,   // Message requesting a file
    ACK,       // Acknowledgment message
    DATA,      // Data message containing file chunks
    COMPLETE,  // Message indicating file transfer is complete
    HANDSHAKE, // Handshake message
    ERROR_,    // Error message
    STATUS,    // Status message
    UNKNOWN    // Unknown message type
};

/*
 *  Peer class
 *  @brief: Represents a peer in the BitTorrent system that can connect to other peers and a tracker.
 */
class Peer
{
public:
    /*
     *  Peer Constructor
     *  @brief: Initializes a Peer instance with the given IP, port, tracker details, and directory paths.
     *  @param ip: The IP address of the peer.
     *  @param port: The port number of the peer.
     *  @param trackerIp: The IP address of the tracker.
     *  @param trackerPort: The port number of the tracker.
     *  @param uploadDir: Directory for uploaded files.
     *  @param downloadDir: Directory for downloaded files.
     */
    Peer(const std::string &ip, int port, const std::string &id, const std::string &trackerIp, int trackerPort,
         const std::string &uploadDir, const std::string &downloadDir);

    // Destructor to clean up resources
    ~Peer();

    /*
     *  connectToPeer
     *  @brief: Connects to another peer using the provided IP and port.
     *  @param incomingPeerIP: The IP address of the peer to connect to.
     *  @param incomingPeerPort: The port number of the peer to connect to.
     *  @return: True if connection is successful; otherwise, false.
     */
    bool connectToPeer(const std::string &incomingPeerIP, int incomingPeerPort);

    /*
     *  sendMessage
     *  @brief: Sends a message to a connected peer.
     *  @param message: The message to be sent.
     *  @param connectedPeerSocket: The socket for the connected peer.
     */
    void sendMessage(const std::string &message, SOCKET connectedPeerSocket);

    /*
     *  disconnectPeer
     *  @brief: Disconnects from a specific peer.
     *  @param peerSocket: The socket for the peer to be disconnected.
     */
    void disconnectPeer(SOCKET peerSocket);

    /*
     *  requestFile
     *  @brief: Requests a specific file from a connected peer.
     *  @param filename: The name of the file to request.
     *  @param peerSocket: The socket for the peer to send the request.
     */
    void requestFile(const std::string &filename, SOCKET peerSocket);

    /*
     *  sendAck
     *  @brief: Sends an acknowledgment message to a connected peer after receiving a file.
     *  @param filename: The name of the file for which acknowledgment is being sent.
     *  @param peerSocket: The socket for the connected peer.
     */
    void sendAck(const std::string &filename, SOCKET peerSocket);

    /*
     *  connectToTracker
     *  @brief: Establishes a connection to the tracker.
     *  @return: True if the connection is successful; otherwise, false.
     */
    bool connectToTracker();

    /*
     *  announceToTracker
     *  @brief: Announces the presence of this peer to the tracker.
     *  @return: True if the announcement is successful; otherwise, false.
     */
    bool announceToTracker();

    /*
     *  requestPeers
     *  @brief: Requests a list of peers from the tracker for a specified file.
     *  @param requiredFile: The name of the file for which to request peers.
     */
    void requestPeers(const std::string &requiredFile);

    /*
     *  receiveMessage
     *  @brief: Receives a message from a connected peer.
     *  @param peerSocket: The socket for the connected peer.
     *  @return: The received message as a string.
     */
    std::string receiveMessage(SOCKET peerSocket);

    /*
     *  getFileSize
     *  @brief: Retrieves the size of the file being shared by this peer.
     *  @return: The size of the file in bytes.
     */
    inline std::size_t getFileSize() const { return fileSize; }

    /*
     *  disconnectAllPeers
     *  @brief: Disconnects from all currently connected peers.
     */
    void disconnectAllPeers();

    /*
     *  start
     *  @brief: Starts the peer to begin handling connections and messages.
     */
    void start();

    /*
     *  handlePeer
     *  @brief: Handles incoming connections from peers.
     *  @param clientFd: The socket file descriptor for the connected client.
     *  @param clientAddr: The address information of the connected client.
     */
    void handlePeer(SOCKET clientFd, sockaddr_in clientAddr);

    /*
     *  isPeerConnected
     *  @brief: Checks if a specific peer is currently connected.
     *  @param peerIP: The IP address of the peer to check.
     *  @param peerPort: The port number of the peer to check.
     *  @return: True if the peer is connected; otherwise, false.
     */
    bool isPeerConnected(const std::string &peerIP, int peerPort);

    /*
     *  sendHandshake
     *  @brief: Sends a handshake message to a connected peer after establishing a connection.
     *  @param connectedPeerSocket: The socket for the connected peer.
     *  @param listeningPort: The port number on which the peer is listening for incoming connections.
     */
    void sendHandshake(SOCKET connectedPeerSocket, int listeningPort, SOCKET listeningSocket);

    // Structure representing a connected peer
    struct PeerInfo
    {
        std::string ip; // Peer IP address
        int port;       // Peer port number
        SOCKET socket;  // The socket for communication
    };

    // Peer's socket for communication with other peers
    SOCKET peerSocket;

    // Vector to store information about connected peers
    std::vector<PeerInfo> connectedPeers;

    // Tracker socket for communicating with the tracker
    SOCKET trackerSocket;

    // Listener thread for incoming messages from peers
    std::thread listenerThread;

    // Map to store active peers with file availability: filename -> vector of (IP, Port) pairs
    std::map<std::string, std::vector<std::pair<std::string, int>>> filePeers;

    // Vector to store threads for other peer connections
    std::vector<std::thread> otherPeerThreads;

    // Atomic flag to indicate if the peer is running
    std::atomic<bool> isRunning;

    // Tracker's IP address
    std::string trackerIP;

    // Tracker's port number
    int trackerPort;

    // Peer's own IP address
    std::string peerIP;

    // Peer's port number
    int peerPort;

    // Peer's id
    std::string peerId;

    // Size of the file being shared
    std::size_t fileSize;

    // Path for uploaded files
    std::string uploadPath;

    // Path for downloaded files
    std::string downloadPath;

    // Mutex for ensuring thread-safe operations
    std::mutex mtx;

    // Mutex to protect shared resources
    std::mutex peerMutex;             // Mutex for protecting peer connections
    std::mutex fileMutex;             // Mutex for protecting file-related operations
    std::ofstream outputDownloadFile; // Ensure you have an output download file stream
    std::ofstream outputUploadFile;   // Ensure you have an output upload file stream
    std::string currentFilename;      // Keep track of the current file being received

    /*
     *  listenForMessages
     *  @brief: Listens for incoming messages on the specified peer socket.
     *  @param peerSocket: The socket to listen for messages.
     */
    void listenForMessages(SOCKET peerSocket);

    /*
     *  getMessageType
     *  @brief: Identifies the type of message received based on its content.
     *  @param message: The message string to analyze.
     *  @return: The MessageType enum representing the type of the message.
     */
    MessageType getMessageType(const std::string &message);

    /*
     *  processMessage
     *  @brief: Processes an incoming message based on its type and content.
     *  @param message: The message string to process.
     *  @param connectedPeerSocket: The socket for the peer that sent the message.
     */
    void processMessage(const std::string &message, SOCKET connectedPeerSocket);

    /*
     *  showConnectedPeers
     *  @brief: Displays the details of all currently connected peers.
     */
    void showConnectedPeers();

    /*
     *  showfilePeers
     *  @brief: Displays the list of peers available for each file.
     */
    void showfilePeers();

    /*
     *  showMyInfo
     *  @brief: Displays the details of the peer itself.
     */
    void showMyInfo();
};

#endif // PEER_H
