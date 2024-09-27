#ifndef TRACKER_H
#define TRACKER_H

#include <iostream>
#include <winsock2.h>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <string>
#include <sstream>    // For stringstream: used in request/response processing
#include <ws2tcpip.h> // For inet_ntop and inet_pton: used for IP address conversion on Windows
#include <atomic>     // For atomic: used to handle tracker shutdown flag

/*
 *  Tracker class
 *  @brief: This class represents a tracker that manages active peers and tracks the files they share.
 *          It handles communication with peers, processes their requests, and responds with peer information.
 */
class Tracker
{
private:
    std::atomic<bool> isRunning;            // Atomic flag to track the running state of the tracker
    SOCKET serverFd;                        // Server socket file descriptor for listening to peer connections
    std::vector<std::thread> clientThreads; // Vector to store threads handling individual clients
    std::mutex mtx;                         // Mutex to protect shared resources like peer data structures
    int serverPort;                         // Port on which the tracker is running
    std::string serverIP;                   // IP address of the tracker server
    bool hasShutdown = false;               // Flag to indicate if the server has been shut down

    // Map to store file peers information: filename -> vector of pairs (IP, Port) of peers that have the file
    std::map<std::string, std::vector<std::pair<std::string, int>>> filePeers;

    // Private method to handle errors by printing a message and terminating the process
    void handleError(const std::string &msg);

public:
    /*
     *  Tracker Constructor
     *  @brief: Initializes the tracker with the given IP and port.
     *  @param ip: IP address of the tracker server.
     *  @param port: Port number on which the tracker listens for connections.
     */
    Tracker(const std::string &ip, int port);

    /*
     *  init
     *  @brief: Initializes the tracker by creating a socket and binding it to the specified IP and port.
     */
    void init();

    /*
     *  start
     *  @brief: Starts the tracker to listen for incoming peer connections and handles them concurrently.
     */
    void start();

    /*
     *  handleClient
     *  @brief: Handles the communication with a single peer. Each client connection is handled in its own thread.
     *  @param clientFd: The socket file descriptor for the connected peer.
     *  @param clientAddr: The address information of the connected peer.
     */
    void handleClient(SOCKET clientFd, sockaddr_in clientAddr);

    /*
     *  processRequest
     *  @brief: Processes incoming requests from a peer, such as announcing a file or requesting peer info.
     *  @param clientFd: The socket file descriptor for the connected peer.
     *  @param request: The request message received from the peer.
     *  @param clientAddr: The address information of the connected peer.
     */
    void processRequest(SOCKET clientFd, const std::string &request, sockaddr_in clientAddr);

    /*
     *  announcePeer
     *  @brief: Handles the "announce" request from a peer to notify the tracker of a file it has.
     *  @param clientFd: The socket file descriptor for the connected peer.
     *  @param filename: The name of the file being announced by the peer.
     *  @param peerPort: The port number of the peer announcing the file.
     *  @param clientAddr: The address information of the connected peer.
     */
    void announcePeer(SOCKET clientFd, const std::string &filename, const std::string &peerPort, sockaddr_in clientAddr);

    /*
     *  sendPeersList
     *  @brief: Sends a list of peers that have a requested file to the requesting peer.
     *  @param clientFd: The socket file descriptor for the connected peer.
     *  @param filename: The name of the file whose peers are being requested.
     */
    void sendPeersList(SOCKET clientFd, const std::string &filename);

    /*
     *  shutdown
     *  @brief: Gracefully shuts down the tracker by closing the server socket and stopping all threads.
     */
    void shutdown();

    /*
     *  getPeers
     *  @brief: Retrieves a list of peers that have a specific file.
     *  @param filename: The name of the file whose peers are being requested.
     *  @return: A vector of (IP, Port) pairs representing the peers that have the file.
     */
    std::vector<std::pair<std::string, int>> getPeers(const std::string &filename);

    /*
     *  getTrackerPort
     *  @brief: Returns the port number on which the tracker is running.
     *  @return: The port number of the tracker.
     */
    int getTrackerPort();

    /*
     *  getTrackerIP
     *  @brief: Returns the IP address of the tracker.
     *  @return: The IP address of the tracker.
     */
    std::string getTrackerIP();

    /*
     *  acceptPeerConnection
     *  @brief: Accepts an incoming connection from a peer.
     *  @return: The socket file descriptor for the connected peer.
     */
    SOCKET acceptPeerConnection();

    /*
     *  Tracker Destructor
     *  @brief: Destructor to clean up resources (e.g., closing sockets, joining threads).
     */
    ~Tracker();
};

#endif // TRACKER_H
