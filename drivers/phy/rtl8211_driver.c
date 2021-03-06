/**
 * @file rtl8211_driver.c
 * @brief RTL8211 Ethernet PHY transceiver
 *
 * @section License
 *
 * Copyright (C) 2010-2017 Oryx Embedded SARL. All rights reserved.
 *
 * This file is part of CycloneTCP Open.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * @author Oryx Embedded SARL (www.oryx-embedded.com)
 * @version 1.8.0
 **/

//Switch to the appropriate trace level
#define TRACE_LEVEL NIC_TRACE_LEVEL

//Dependencies
#include "core/net.h"
#include "drivers/phy/rtl8211_driver.h"
#include "debug.h"


/**
 * @brief RTL8211 Ethernet PHY driver
 **/

const PhyDriver rtl8211PhyDriver =
{
   rtl8211Init,
   rtl8211Tick,
   rtl8211EnableIrq,
   rtl8211DisableIrq,
   rtl8211EventHandler,
};


/**
 * @brief RTL8211 PHY transceiver initialization
 * @param[in] interface Underlying network interface
 * @return Error code
 **/

error_t rtl8211Init(NetInterface *interface)
{
   //Debug message
   TRACE_INFO("Initializing RTL8211...\r\n");

   //Initialize external interrupt line driver
   if(interface->extIntDriver != NULL)
      interface->extIntDriver->init();

   //Reset PHY transceiver
   rtl8211WritePhyReg(interface, RTL8211_PHY_REG_BMCR, BMCR_RESET);
   //Wait for the reset to complete
   while(rtl8211ReadPhyReg(interface, RTL8211_PHY_REG_BMCR) & BMCR_RESET);

   //Dump PHY registers for debugging purpose
   rtl8211DumpPhyReg(interface);

   //The PHY will generate interrupts when link status changes are detected
   rtl8211WritePhyReg(interface, RTL8211_PHY_REG_INER, INER_AN_COMPLETE | INER_LINK_STATUS);

   //Force the TCP/IP stack to poll the link state at startup
   interface->phyEvent = TRUE;
   //Notify the TCP/IP stack of the event
   osSetEvent(&netEvent);

   //Successful initialization
   return NO_ERROR;
}


/**
 * @brief RTL8211 timer handler
 * @param[in] interface Underlying network interface
 **/

void rtl8211Tick(NetInterface *interface)
{
   uint16_t value;
   bool_t linkState;

   //No external interrupt line driver?
   if(interface->extIntDriver == NULL)
   {
      //Read basic status register
      value = rtl8211ReadPhyReg(interface, RTL8211_PHY_REG_BMSR);
      //Retrieve current link state
      linkState = (value & BMSR_LINK_STATUS) ? TRUE : FALSE;

      //Link up event?
      if(linkState && !interface->linkState)
      {
         //Set event flag
         interface->phyEvent = TRUE;
         //Notify the TCP/IP stack of the event
         osSetEvent(&netEvent);
      }
      //Link down event?
      else if(!linkState && interface->linkState)
      {
         //Set event flag
         interface->phyEvent = TRUE;
         //Notify the TCP/IP stack of the event
         osSetEvent(&netEvent);
      }
   }
}


/**
 * @brief Enable interrupts
 * @param[in] interface Underlying network interface
 **/

void rtl8211EnableIrq(NetInterface *interface)
{
   //Enable PHY transceiver interrupts
   if(interface->extIntDriver != NULL)
      interface->extIntDriver->enableIrq();
}


/**
 * @brief Disable interrupts
 * @param[in] interface Underlying network interface
 **/

void rtl8211DisableIrq(NetInterface *interface)
{
   //Disable PHY transceiver interrupts
   if(interface->extIntDriver != NULL)
      interface->extIntDriver->disableIrq();
}


/**
 * @brief RTL8211 event handler
 * @param[in] interface Underlying network interface
 **/

void rtl8211EventHandler(NetInterface *interface)
{
   uint16_t status;

   //Read status register to acknowledge the interrupt
   status = rtl8211ReadPhyReg(interface, RTL8211_PHY_REG_INSR);

   //Link status change?
   if(status & (INSR_AN_COMPLETE | INSR_LINK_STATUS))
   {
      //Any link failure condition is latched in the BMSR register... Reading
      //the register twice will always return the actual link status
      status = rtl8211ReadPhyReg(interface, RTL8211_PHY_REG_BMSR);
      status = rtl8211ReadPhyReg(interface, RTL8211_PHY_REG_BMSR);

      //Link is up?
      if(status & BMSR_LINK_STATUS)
      {
         //Read PHY status register
         status = rtl8211ReadPhyReg(interface, RTL8211_PHY_REG_PHYSR);

         //Check current speed
         switch(status & PHYSR_SPEED_MASK)
         {
         //10BASE-T
         case PHYSR_SPEED_10:
            interface->linkSpeed = NIC_LINK_SPEED_10MBPS;
            break;
         //100BASE-TX
         case PHYSR_SPEED_100:
            interface->linkSpeed = NIC_LINK_SPEED_100MBPS;
            break;
         //1000BASE-T
         case PHYSR_SPEED_1000:
            interface->linkSpeed = NIC_LINK_SPEED_1GBPS;
            break;
         //Unknown speed
         default:
            //Debug message
            TRACE_WARNING("Invalid speed\r\n");
            break;
         }

         //Check current duplex mode
         if(status & PHYSR_DUPLEX)
            interface->duplexMode = NIC_FULL_DUPLEX_MODE;
         else
            interface->duplexMode = NIC_HALF_DUPLEX_MODE;

         //Update link state
         interface->linkState = TRUE;

         //Adjust MAC configuration parameters for proper operation
         interface->nicDriver->updateMacConfig(interface);
      }
      else
      {
         //Update link state
         interface->linkState = FALSE;
      }

      //Process link state change event
      nicNotifyLinkChange(interface);
   }
}


/**
 * @brief Write PHY register
 * @param[in] interface Underlying network interface
 * @param[in] address PHY register address
 * @param[in] data Register value
 **/

void rtl8211WritePhyReg(NetInterface *interface, uint8_t address, uint16_t data)
{
   uint8_t phyAddr;

   //Get the address of the PHY transceiver
   if(interface->phyAddr < 32)
      phyAddr = interface->phyAddr;
   else
      phyAddr = RTL8211_PHY_ADDR;

   //Write the specified PHY register
   interface->nicDriver->writePhyReg(phyAddr, address, data);
}


/**
 * @brief Read PHY register
 * @param[in] interface Underlying network interface
 * @param[in] address PHY register address
 * @return Register value
 **/

uint16_t rtl8211ReadPhyReg(NetInterface *interface, uint8_t address)
{
   uint8_t phyAddr;

   //Get the address of the PHY transceiver
   if(interface->phyAddr < 32)
      phyAddr = interface->phyAddr;
   else
      phyAddr = RTL8211_PHY_ADDR;

   //Read the specified PHY register
   return interface->nicDriver->readPhyReg(phyAddr, address);
}


/**
 * @brief Dump PHY registers for debugging purpose
 * @param[in] interface Underlying network interface
 **/

void rtl8211DumpPhyReg(NetInterface *interface)
{
   uint8_t i;

   //Loop through PHY registers
   for(i = 0; i < 32; i++)
   {
      //Display current PHY register
      TRACE_DEBUG("%02" PRIu8 ": 0x%04" PRIX16 "\r\n", i, rtl8211ReadPhyReg(interface, i));
   }

   //Terminate with a line feed
   TRACE_DEBUG("\r\n");
}
