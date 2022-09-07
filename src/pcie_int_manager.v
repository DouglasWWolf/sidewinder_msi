`timescale 1ns / 1ps

//====================================================================================
//                        ------->  Revision History  <------
//====================================================================================
//
//   Date     Who   Ver  Changes
//====================================================================================
// 06-Sep-22  DWW  1000  Initial creation
//====================================================================================

/*

Usage:

   This module is an AXI4-Lite slave that allows an application to generate or clear
   PCIe interrupts using the MSI mechanism.

   We support up to 32 interrupt sources, number 0 thru 31

   To use:
       Register 0:  write = Sets the IRQ bits for the specified sources
                    read  = Returns the current set of IRQ bits
    
       Register 1:  write = Clears the IRQ bits for the specified sources
                    read  = Returns the count of interrupt that have been generated

    There are also four input ports IRQ0_IN, IRQ1_IN, IRQ2_IN, and IRQ3_IN.  Strobing
    any of those input lines sets the relevant IRQ output line high.   This provides
    a mechanism for raising an IRQ line that doesn't require an AXI transaction.

    In order to be able to generate MSI interrupts, the PCIe bridge must have MSI
    interrupts and bus-master mode both enabled by the PC host  You can enable MSI
    interrupts in Linux by issuing the command:
          sudo setpci -s <bdf_of_the_card> 4a.w=1

    Example: if the bdf of the PCIe device is 65:00.0, enable MSI interrupts with:
          sudo setpci -s 65:00.0 4a.w=1

    Busmaster mode can be enabled via:
          sudo setpci -s <bdf_of_the_card> COMMAND=0106

*/


module pcie_int_manager
(
    input clk, resetn,
    
    output IRQ_REQ,
    input  IRQ_ACK,

    input  IRQ0_IN, IRQ1_IN, IRQ2_IN, IRQ3_IN,


    //================== This is an AXI4-Lite slave interface ==================
        
    // "Specify write address"              -- Master --    -- Slave --
    input[31:0]                             S_AXI_AWADDR,   
    input                                   S_AXI_AWVALID,  
    output                                                  S_AXI_AWREADY,
    input[2:0]                              S_AXI_AWPROT,

    // "Write Data"                         -- Master --    -- Slave --
    input[31:0]                             S_AXI_WDATA,      
    input                                   S_AXI_WVALID,
    input[3:0]                              S_AXI_WSTRB,
    output                                                  S_AXI_WREADY,

    // "Send Write Response"                -- Master --    -- Slave --
    output[1:0]                                             S_AXI_BRESP,
    output                                                  S_AXI_BVALID,
    input                                   S_AXI_BREADY,

    // "Specify read address"               -- Master --    -- Slave --
    input[31:0]                             S_AXI_ARADDR,     
    input                                   S_AXI_ARVALID,
    input[2:0]                              S_AXI_ARPROT,     
    output                                                  S_AXI_ARREADY,

    // "Read data back to master"           -- Master --    -- Slave --
    output[31:0]                                            S_AXI_RDATA,
    output                                                  S_AXI_RVALID,
    output[1:0]                                             S_AXI_RRESP,
    input                                   S_AXI_RREADY
    //==========================================================================
 );

    //==========================================================================
    // We'll communicate with the AXI4-Lite Slave core with these signals.
    //==========================================================================
    // AXI Slave Handler Interface for write requests
    wire[31:0]  ashi_waddr;     // Input:  Write-address
    wire[31:0]  ashi_wdata;     // Input:  Write-data
    wire        ashi_write;     // Input:  1 = Handle a write request
    reg[1:0]    ashi_wresp;     // Output: Write-response (OKAY, DECERR, SLVERR)
    wire        ashi_widle;     // Output: 1 = Write state machine is idle

    // AXI Slave Handler Interface for read requests
    wire[31:0]  ashi_raddr;     // Input:  Read-address
    wire        ashi_read;      // Input:  1 = Handle a read request
    reg[31:0]   ashi_rdata;     // Output: Read data
    reg[1:0]    ashi_rresp;     // Output: Read-response (OKAY, DECERR, SLVERR);
    wire        ashi_ridle;     // Output: 1 = Read state machine is idle
    //==========================================================================

    // The state of our two state machines
    reg[2:0] read_state, write_state;

    // The state machines are idle when they're in state 0 when their "start" signals are low
    assign ashi_widle = (ashi_write == 0) && (write_state == 0);
    assign ashi_ridle = (ashi_read  == 0) && (read_state  == 0);

    // This is a bitmap of the 32 interrupt sources
    reg[31:0] irq_map;

    // The IRQ_REQ output is high so long as any interrupt source is high
    assign IRQ_REQ = (irq_map != 0);

    // This is a count of the number of IRQ acknowledgements we've received
    reg[31:0] interrupt_count;

    // These are the valid values for ashi_rresp and ashi_wresp
    localparam OKAY   = 0;
    localparam SLVERR = 2;
    localparam DECERR = 3;

    // An AXI slave is gauranteed a minimum of 128 bytes of address space
    // (128 bytes is 32 32-bit registers)
    localparam ADDR_MASK = 7'h7F;


    //==========================================================================
    // Here we are counting the number of times the PCIe core tells us that
    // it has passed our interrupt request up to the host PC
    //==========================================================================
    always @(posedge clk) begin
        if (resetn == 0)
            interrupt_count <= 0;
        else if (IRQ_ACK)
            interrupt_count <= interrupt_count + 1;
    end
    //==========================================================================


    //==========================================================================
    // World's simplest state machine for handling write requests
    //==========================================================================
    always @(posedge clk) begin

        // If we're in reset, initialize important registers
        if (resetn == 0) begin
            write_state <= 0;
            irq_map     <= 0;
        
        // If we're not in reset
        end else begin

            // If a write-request has come in...
            if (ashi_write) begin

                // Assume for a moment that we will be reporting "OKAY" as a write-response
                ashi_wresp <= OKAY;

                case((ashi_waddr & ADDR_MASK) >> 2)
                    // If the user wants to raise an IRQ line...
                    0:  irq_map <= irq_map | ashi_wdata;

                    // If the user wants to lower an IRQ line...
                    1:  irq_map <= irq_map & ~ashi_wdata;

                    // A write to any other address is a slave-error
                    default: ashi_wresp <= SLVERR;
                endcase
            end

            // If one of the IRQ input lines go high, set that bit in the irq_map
            if (IRQ0_IN) irq_map[0] <= 1;
            if (IRQ1_IN) irq_map[1] <= 1;
            if (IRQ2_IN) irq_map[2] <= 1;
            if (IRQ3_IN) irq_map[3] <= 1;
        end
    end
    //==========================================================================


 
    //==========================================================================
    // World's simplest state machine for handling read requests
    //==========================================================================
    always @(posedge clk) begin

        // If we're in reset, initialize important registers
        if (resetn == 0) begin
            read_state <= 0;
        
        // If we're not in reset, and a read-request has occured...        
        end else if (ashi_read) begin
        
            // We'll always acknowledge the read as valid
            ashi_rresp <= OKAY;

            case((ashi_raddr & ADDR_MASK) >> 2)
                // If the user wants to read the IRQ map
                0:  ashi_rdata <= irq_map;

                // If the user wants to read the number of interrupts generated...
                1:  ashi_rdata <= interrupt_count;

                // A read of any other address returns 0
                default: ashi_rdata <= 0;
            endcase
        end
    end
    //==========================================================================





    //==========================================================================
    // This connects us to an AXI4-Lite slave core
    //==========================================================================
    axi4_lite_slave axi_slave
    (
        .clk            (clk),
        .resetn         (resetn),
        
        // AXI AW channel
        .AXI_AWADDR     (S_AXI_AWADDR),
        .AXI_AWVALID    (S_AXI_AWVALID),   
        .AXI_AWPROT     (S_AXI_AWPROT),
        .AXI_AWREADY    (S_AXI_AWREADY),
        
        // AXI W channel
        .AXI_WDATA      (S_AXI_WDATA),
        .AXI_WVALID     (S_AXI_WVALID),
        .AXI_WSTRB      (S_AXI_WSTRB),
        .AXI_WREADY     (S_AXI_WREADY),

        // AXI B channel
        .AXI_BRESP      (S_AXI_BRESP),
        .AXI_BVALID     (S_AXI_BVALID),
        .AXI_BREADY     (S_AXI_BREADY),

        // AXI AR channel
        .AXI_ARADDR     (S_AXI_ARADDR), 
        .AXI_ARVALID    (S_AXI_ARVALID),
        .AXI_ARPROT     (S_AXI_ARPROT),
        .AXI_ARREADY    (S_AXI_ARREADY),

        // AXI R channel
        .AXI_RDATA      (S_AXI_RDATA),
        .AXI_RVALID     (S_AXI_RVALID),
        .AXI_RRESP      (S_AXI_RRESP),
        .AXI_RREADY     (S_AXI_RREADY),

        // ASHI write-request registers
        .ASHI_WADDR     (ashi_waddr),
        .ASHI_WDATA     (ashi_wdata),
        .ASHI_WRITE     (ashi_write),
        .ASHI_WRESP     (ashi_wresp),
        .ASHI_WIDLE     (ashi_widle),

        // AMCI-read registers
        .ASHI_RADDR     (ashi_raddr),
        .ASHI_RDATA     (ashi_rdata),
        .ASHI_READ      (ashi_read ),
        .ASHI_RRESP     (ashi_rresp),
        .ASHI_RIDLE     (ashi_ridle)
    );
    //==========================================================================

endmodule






