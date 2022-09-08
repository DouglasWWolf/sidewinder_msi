//==========================================================================================================
// distributor.cpp - Implements a mechanism for distributing interrupt notifications
//==========================================================================================================
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>
#include "distributor.h"

// We want the entire std:: namespace
using namespace std;

// This is a place to throw away values
static volatile int bitBucket;

//==========================================================================================================
// createPipe() - Creates a FIFO and opens it
//==========================================================================================================
static int createPipe(const char* path, int n)
{
    char name[256];

    // Create the name of this pipe
    sprintf(name, "%s%i", path, n);

    // If it already exists, get rid of it
    remove(name);

    // Create the FIFO
    if (mkfifo(name, 0666) != 0)
    {
        fprintf(stderr, "Failed to make fifo %s\n", name);
        return -1;
    }

    // Open the FIFO
    int fd = open(name, O_RDWR);
    if (fd < 0)
    {
        fprintf(stderr, "Failed to open fifo %s\n", name);
        return -1;
    }

    // If we get here, we have a valid file descriptor and it's open
    return fd;
}
//==========================================================================================================


//==========================================================================================================
// openPipe() - Opens a FIFO for reading
//==========================================================================================================
static int openPipe(const char* path, int n)
{
    char name[256];

    // Create the name of this pipe
    sprintf(name, "%s%i", path, n);

    // Open the FIFO
    int fd = open(name, O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "Failed to open fifo %s\n", name);
        exit(1);
    }

    // If we get here, we have a valid file descriptor and it's open
    return fd;
}
//==========================================================================================================



//==========================================================================================================
// Constructor - Ensures that all file descriptors start out at -1
//==========================================================================================================
CDistributor::CDistributor()
{
    // Initialize all the file descriptors to "not open"
    for (int i=0; i<MAX_IRQS; ++i) fd_[i] = -1;

    // And there are no file descriptors open
    irqCount_ = 0;
}
//==========================================================================================================


//==========================================================================================================
// init() - Initialize the distribution system (create the FIFOs, etc)
//
// On Exit: path_      = The full pathname of the FIFOs (except for the number on the end)
//          irqCount_  = The number of interrupt sources to manage
//          fd_[]      = Array of file descriptors for the write-end of our FIFOs
//          highestFD_ = The largest value in the fd_[] array
//
// On exit, all FIFOs have been created and opened.             
//==========================================================================================================
bool CDistributor::init(string dir, int irqCount)
{
    // Construct the portion of the FIFO names that isn't a number
    path_ = dir + "/interrupt";

    // Save the IRQ count for posterity
    irqCount_ = irqCount;

    // We don't yet know what our highest file-descriptor is
    highestFD_ = -1;

    // Loop through each interrupt source we need to support...
    for (int i=0; i<irqCount; ++i)
    {
        // Create and open the FIFO for this interrupt source
        fd_[i] = createPipe(path_.c_str(), i);

        // If we couldn't create/open this FIFO, tell the caller
        if (fd_[i] == -1) return false;

        // Keep track of what our highest file descriptor is
        if (fd_[i] > highestFD_) highestFD_ = fd_[i];
    }

    // If we get here, tell the caller that all is well
    return true;
}
//==========================================================================================================



//==========================================================================================================
// distribute() - Writes one byte to the FIFO of each active interrupt source
//==========================================================================================================
void CDistributor::distribute(int irqSources)
{
    fd_set wfds;
    int    i;

    // We want our select() statement to return immediately
    static timeval timeout = {0,0};

    // Clear our file descriptor set
    FD_ZERO(&wfds);

    // wfds is an fd_set of each file descriptor we're interested in
    for (i=0; i<irqCount_; ++i) FD_SET(fd_[i], &wfds);

    // Find out which file descriptors can be written to without blocking
    select(highestFD_+1, NULL, &wfds, NULL, &timeout);

    // Loop through each possible interrupt source...
    for (i=0; i<irqCount_; ++i)
    {
        // Fetch the file descriptor for this interrupt source...
        int fd = fd_[i];
        
        // If we need to distribute this interrupt and the FIFO can be 
        // writen to without blocking, write a byte to the FIFO
        if (irqSources & 1 << i)
        {
            if (FD_ISSET(fd, &wfds)) bitBucket = write(fd, "X", 1);
        }
    }    
}
//==========================================================================================================


//==========================================================================================================
// cleanup() - Closes all of the file descriptors and deletes all of the FIFOs
//==========================================================================================================
void CDistributor::cleanup()
{
    int i;
    char filename[256];

    // Close all of the file descriptors
    for (i=0; i<irqCount_; ++i) 
    {
        int& fd = fd_[i];
        if (fd != -1) close(fd);
        fd = -1;
    }

    // If there's no FIFO path specified, we're done
    if (path_.empty()) return;

    // Get the pathname as a const char*
    const char* path = path_.c_str();

    // And remove every possible FIFO
    for (i=0; i<MAX_IRQS; ++i)
    {
        sprintf(filename, "%s%d", path, i);
        remove(filename);
    }
}
//==========================================================================================================



//==========================================================================================================
// launchTest() - Launches "selfTest" in its own thread
//==========================================================================================================
void CDistributor::spawnSelfTest(volatile uint32_t* imReg0)
{
    // If we haven't been initialized, don't do anything
    if (irqCount_ == 0) return;

    // Spawn "selfTest()" in it's own thread
    thread thread(&CDistributor::selfTest, this, imReg0);

    // Let it keep running, even when "thread" goes out of scope
    thread.detach();
}
//==========================================================================================================


//==========================================================================================================
// selfTest() - This threads sits in a loop, sending "generate an interrupt" commands (via the AXI/PCIe 
//              bridge), then doing a blocking read on the appropriate FIFO to confirm that the interrupt
//              actually occured
//==========================================================================================================
void CDistributor::selfTest(volatile uint32_t* imReg0)
{
    int fd[MAX_IRQS];
    char c[1];

    // A counter of how many tests we've done
    uint32_t counter = 0;

    // Get a const char* to the FIFO root name
    const char* path = path_.c_str();

    // Open each FIFO
    for (int i=0; i<irqCount_; ++i)
    {
        fd[i] = openPipe(path, i);
    }

    // This is the interrupt source we're going to test
    int irq = 0;

    // We're going to generate interrupts and sense them until the process is killed
    while (true)
    {
        // Display a constantly changing message
        printf("Generating interrupt #%d on irq %d\n", ++counter, irq);

        // Generate the interrupt
        *imReg0 = (1 << irq);

        // Wait for the interrupt notification
        int bytesRead = read(fd[irq], c, 1);

        // If the other side of the pipe was closed, it means the test is done
        if (bytesRead == 0) break;

        // We should <always> read exactly 1 byte
        if (bytesRead != 1)
        {
            printf("bytesRead was %d!\n", bytesRead);
            exit(1);            
        }

        // The next test is for the next interrupt source
        if (++irq == irqCount_) irq = 0;
    }
}
//==========================================================================================================
