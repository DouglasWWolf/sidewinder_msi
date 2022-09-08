#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <filesystem>
#include "distributor.h"
#include "PciDevice.h"

using namespace std;

static volatile int bitBucket;

void monitorInterrupts(int uioDevice);
bool initializePCI();
void parseCommandLine(const char** argv);
void signalHandler(int sigNumber);
int  initializeUIO(string device);

// Configuration parameters from the command line
struct conf_t
{
    string   device;
    string   dirName;
    int      irqCount;
    uint32_t axiAddr;
    bool     selfTest;
    bool     verbose;
} conf;


CDistributor Distributor;
PciDevice    PCI;

volatile uint32_t *imReg0;
volatile uint32_t *imReg1;


//=================================================================================================
// main() - This program uses the uio_pci_generic driver as the basis for a userspace driver
//          that creates one FIFO per interrupt source and writes 1 byte to that FIFO every 
//          time that source generates an interrupt
//=================================================================================================
int main(int argc, const char** argv)
{
    // Set some default configuration parameters
    conf.irqCount = 1;
    conf.dirName = ".";
    conf.device  = "10ee:903f";
    conf.axiAddr = 0x4000;
    conf.verbose = false;

    // Tell Linux which signals we'd like to handle
    signal(SIGINT, signalHandler);

    // Parse configuration parameters from the command line
    parseCommandLine(argv);

    // If we're not running with root priveleges, give up
    if (geteuid() != 0)
    {
        fprintf(stderr, "Must be root to run.  Use sudo.\n");
        exit(1);        
    }

    // Initialize the Linux UIO subsystem
    int uioIndex = initializeUIO(conf.device);

    // Initalize PCI, and if it fails, bail out
    if (!initializePCI()) exit(1);

    // Initialize the interrupt distributor and if it fails, bail out
    if (!Distributor.init(conf.dirName, conf.irqCount)) exit(1);

    // If we're supposed to spawn the self-test thread, make it so
    if (conf.selfTest) Distributor.spawnSelfTest(imReg0);

    // Monitor and distribute interrupts
    monitorInterrupts(uioIndex);
}
//=================================================================================================


//=================================================================================================
// monitorInterrupts() - Sits in a loop reading interrupt notifications and distributing 
//                       notifications to the FIFOs that track each interrupt source
//=================================================================================================
void monitorInterrupts(int uioDevice)
{
    int      uiofd;
    int      configfd;
    int      err;
    uint32_t interruptCount;
    uint8_t  commandHigh;
    char     filename[64];

    // Generate the filename of the psudeo-file that notifies us of interrupts
    sprintf(filename, "/dev/uio%d", uioDevice);

    // Open the psuedo-file that notifies us of interrupts
    uiofd = open(filename, O_RDONLY);
    if (uiofd < 0)
    {
        perror("uio open:");
        exit(1);
    }

    // Generate the filename of the PCI config-space psuedo-file
    sprintf(filename, "/sys/class/uio/uio%d/device/config", uioDevice);

    // Open the file that gives us access to the PCI device's confiuration space
    configfd = open(filename, O_RDWR);
    if (configfd < 0)
    {
        perror("config open:");
        exit(1);
    }

    // Fetch the upper byte of the PCI configuration space command word
    err = pread(configfd, &commandHigh, 1, 5);
    if (err != 1)
    {
        perror("command config read:");
        exit(1);
    }
    
    // Turn off the "Disable interrupts" flag
    commandHigh &= ~0x4;

    // Give the user an opportunity to see that we've succesfully started
    printf("Starting uio driver for device %d\n", uioDevice);

    // Loop forever, monitoring incoming interrupt notifications
    while (true)
    {
        // Enable (or re-enable) interrupts
        err = pwrite(configfd, &commandHigh, 1, 5);
        if (err != 1)
        {
            perror("config write:");
            exit(1);
        }

        // Wait for notification that an interrupt has occured
        err = read(uiofd, &interruptCount, 4);
        if (err != 4)
        {
            perror("uio read:");
            exit(1);
        }

        // Fetch the bitmap of active interrupt sources
        uint32_t intSources = *imReg0;

        // If there are no interrupt sources, ignore this interrupt
        if (intSources == 0) continue;

        // If we're in verbose mode, tell the world that an interrupt occured
        if (conf.verbose) printf("Interrupt from sources 0x%08x\n", intSources);

        // Clear the interrupts from those sources
        *imReg1 = intSources;

        // And distribute the interrupt notifications to the FIFOs
        Distributor.distribute(intSources);
    }
}
//=================================================================================================




//=================================================================================================
// initializePCI() - Map the interrupt manager's status/control registers into userspace
//=================================================================================================
bool initializePCI()
{
    // Find the colon in the device name
    const char* colon = strchr(conf.device.c_str(), ':');

    // If there was no colon in the device name, we fail
    if (colon == nullptr) return false;

    // Extract the vendor ID from the device name
    int vendorID = strtoul(conf.device.c_str(), nullptr, 16);

    // Extract the device ID from the device name
    int deviceID = strtoul(colon + 1, nullptr, 16);

    try
    {
        // Memory map the regions of the specified PCI device
        PCI.open(vendorID, deviceID);
    }
    catch(const exception& e)
    {
        fprintf(stderr, "%s\n", e.what());
        return false;
    }

    // Find out how many memory mapped regions this PCI device has
    int regions = PCI.resourceList().size();
        
    // Make sure we have exactly the two regions we are expecting!
    if (regions != 2)
    {
        fprintf(stderr, "This device has the wrong number of regions\n");
        return false;
    }
        
    // Fetch the user-space address of the first region, where our AXI registers are mapped        
    auto baseAddr = PCI.resourceList()[0].baseAddr;

    // Compute the addresses of the two control/status registers of the interrupt manager
    imReg0 = (uint32_t*)(baseAddr + conf.axiAddr + 0x0);
    imReg1 = (uint32_t*)(baseAddr + conf.axiAddr + 0x4);

    // And tell the caller that all is well
    return true;
}
//=================================================================================================




//=================================================================================================
// showHelp() - Displays help text to the user
//=================================================================================================
void showHelp()
{
    printf("options:\n");
    printf(" -device <vendor_id:device_id>\n");
    printf(" -dir <fifo_directory_name>\n");
    printf(" -vectors <# of irq sources>\n");
    printf(" -axi <AXI interrupt manager base address>\n");
    printf(" -selftest\n");
    printf(" -verbose\n");
    exit(1);
}
//=================================================================================================



//=================================================================================================
// parseCommandLine() - Parses the command line, filling in the "conf" structure
//=================================================================================================
void parseCommandLine(const char** argv)
{
    int idx = 0;

    while (true)
    {
        // Fetch the next command-line token
        const char* token = argv[++idx];

        // If we've hit the end of the list, we're done
        if (token == nullptr) break;

        // For convenience, convert that token to a string
        string option = token;

        // Assume for a moment that this option has no argument
        string arg = "";

        // If there's an argument available fetch it    
        if (argv[idx+1] && *argv[idx+1] != '-') arg = argv[++idx];

        if (option == "-device")
            conf.device = arg;
        else if (option == "-dir")
            conf.dirName = arg;
        else if (option == "-vectors")
            conf.irqCount = stoi(arg, 0, 0);
        else if (option == "-axi")
            conf.axiAddr = stoi(arg, 0, 0);
        else if (option == "-selftest")
            conf.selfTest = true;
        else if (option == "-verbose")
            conf.verbose = true;
        else
            showHelp();
    }
}
//=================================================================================================




//=================================================================================================
// signalHandler() - Calls exit() when a SIGINT is intercepted
//=================================================================================================
void signalHandler(int sigNumber)
{
    // Calling exit() gives the destructors in statically declared
    // objects a chance to run
    if (sigNumber == SIGINT) exit(1);
}
//=================================================================================================
