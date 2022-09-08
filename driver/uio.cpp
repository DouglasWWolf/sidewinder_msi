#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <filesystem>
#include <string>

using namespace std;

static volatile int bitBucket;

//=================================================================================================
// getBDF() - Converts a <vendorID:deviceID> into a PCI BDF
//=================================================================================================
static string getBDF(string device)
{
    char command[64];
    char buffer[256];
    string retval = "0000:";

    // Build the command "lspci -d {device}"
    sprintf(command, "lspci -d %s", device.c_str());

    // Open the lpsci command as a process
    FILE* fp = popen(command, "r");

    // If we couldn't do that, give up
    if (fp == nullptr) return "";

    // Fetch the first line of the output
    if (!fgets(buffer, sizeof buffer, fp))
    {
        fclose(fp);
        return "";
    }

    // Find the first space in the line
    char* p = strchr(buffer, ' ');
    if (p == nullptr)
    {
        fclose(fp);
        return "";
    }

    // Terminate the string at the first space. 
    *p = 0;
    
    // The PCI BDF of the device is the first token in the buffer
    return retval + buffer;
}
//=================================================================================================


//=================================================================================================
// registerUioDevice() - Registers our device with the Linux UIO subsystem
//=================================================================================================
static void registerUioDevice(string device)
{
    char buffer[100];

    const char* filename = "/sys/bus/pci/drivers/uio_pci_generic/new_id";

    // Get a copy of our device name in vendorID:deviceID format
    strcpy(buffer, device.c_str());

    // Replace the ':' delimeter with a space
    char* delimeter = strchr(buffer, ':');
    if (delimeter == nullptr) return;
    *delimeter = ' ';

    // Append a linefeed to the end
    strcat(buffer, "\n");

    // Open the psuedo-file that allows us to register a new device
    int fd = open(filename, O_WRONLY);

    // If we can't open it, something is awry
    if (fd == -1)
    {
        fprintf(stderr, "Cant open %s\n", filename);
        return;
    }

    // It doesn't matter if this call fails because the device is already registered
    bitBucket = write(fd, buffer, strlen(buffer));

    // We're done with the file descriptor
    close(fd);
}
//=================================================================================================


//=================================================================================================
// extractIndexFromUioName() - A UIO name is something like "/sys/class/uio/uio3".
//                             This routine returns the numeric value at the end of the string
//=================================================================================================
static int extractIndexFromUioName(string name)
{
    // Find the nul at the end of the string
    const char* p = strchr(name.c_str(), 0);

    // Walk backwards until we encounter a character that isn't a digit
    while (*(p-1) >= '0' && *(p-1) <= '9') --p;

    // And hand the caller the integer value at the end of the string
    return atoi(p);
}
//=================================================================================================


//=================================================================================================
// findUioIndex() - Returns the UIO index of our device
//=================================================================================================
static int findUioIndex(string bdf)
{
    string directory = "/sys/class/uio";

    // We're looking for a symlink target that contains "/<bdf>/uio/uio"
    string searchKey = "/" + bdf + "/uio/uio";

    // Loop through the entry for each device in the specified directory...
    for (auto const& entry : filesystem::directory_iterator(directory)) 
    {
        // Ignore any entry that isn't a symbolic link
        if (!entry.is_symlink()) continue;

        // Fetch the path of the source of the symlink
        filesystem::path source = entry.path();

        // Find the target of the symlink
        filesystem::path target = filesystem::read_symlink(source);

        // Get the filename (or folder name) of the target
        string targetFilename = target.string();

        // If we found the key we're looking for, return the associated UIO index
        if (targetFilename.find(searchKey) != string::npos)
        {
            return extractIndexFromUioName(source.string());
        }
    }

    // If we get here, we couldn't find the BDF we were looking for
    return -1;
}
//=================================================================================================



//=================================================================================================
// initializeUIO() - Registers our device with the Linux UIO subsystem and returns the 
//                   UIO index that corresponds to our device
//
// Passed: device = PCI device name in vendorID:deviceID format
//=================================================================================================
int initializeUIO(string device)
{
    // Convert the device ID into a BDF
    string bdf = getBDF(device);

    // If this device isn't installed, drop dead
    if (bdf.empty())
    {
        fprintf(stderr, "PCI device %s not found\n", device.c_str());
        exit(1);
    }

    // Make sure the generic UIO PCI device driver is loaded
    bitBucket = system("modprobe uio_pci_generic");

    // Register our device with the UIO subsystem
    registerUioDevice(device);

    // Fetch the UIO index that corresponds to our device
    int index = findUioIndex(bdf);

    // If we couldn't find a valid index, complain and give up
    if (index < 0)
    {
        fprintf(stderr, "Can't initialize UIO subsystem for device %s\n", device.c_str());
        exit(1);        
    }

    // The UIO subsystem is initialized.  Hand the caller the UIO index of their device
    return index;
}
//=================================================================================================
