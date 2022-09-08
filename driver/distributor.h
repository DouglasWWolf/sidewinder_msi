
//==========================================================================================================
// distributor.h - Defines a mechanism for distributing interrupt notifications
//==========================================================================================================
#include <string>

class CDistributor
{
public:

    // Default constructor
    CDistributor();
    
    // Default destructor
    ~CDistributor() {cleanup();}

    // Call this to perform all initialization and create the FIFOs
    bool    init(std::string dir, int irqCount);

    // Call this to read an irqSources bitmap and write to the appropriate FIFOs
    void    distribute(int irqSources);

    // Launches a thread that generates interrupts and reads the appropriate
    // FIFO to ensure that the interrupt made it's way up to our software
    void    spawnSelfTest(volatile uint32_t* imReg0);

    // Closes all of the file descriptors and deletes all of the FIFOs
    void    cleanup();

protected:

    // When "spawnSelfTest()" gets called, this is the routine that gets spawned
    void    selfTest(volatile uint32_t* imReg0);

    // Maximum number of interrupt request sources we can support
    enum {MAX_IRQS = 32};

    // One potential file descriptor for each interrupt source we support
    int fd_[MAX_IRQS];

    // This is the largest file descriptor;
    int highestFD_;

    // This is the numbef of interrupt sources that are in use
    int irqCount_;

    // The name of the directory where our FIFOs will reside
    std::string path_;
};

