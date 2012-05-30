// Freescale Headers
#include <hidef.h>      		/* common defines and macros */
#include "derivative.h"      /* derivative-specific definitions */

// C STD Headers
#include <string.h>
#include <stdio.h>

// BMS specific Headers
#include "defines.h"
#include "ltc6802.h"
#include "CANSlave.h"
#include "temp.h"


#define CAN_ENABLE
#define SPI_ENABLE


/*********************
   Global variables
*********************/


//-----------
// Variables
//-----------

uint32  gElapsedTime = 0;	            // le temps en secondes ecoule depuis l'activation du timer PIT0				
uint8   gSlaveID = 0;                   // le num�ro d'identification du module, lu sur le port E.
uint16  gVoltages[NB_CELL];	            // table containing the Cell Voltages in mV
int     gTemp[NB_CELL];         	    // table contenant les temperatures des cellules (300 = 30.0 oC)
uint16  gBalThres = 0;          	    // Balancing target voltage
uint16  gBalanceVector = 0;             // The n-th bit indicates the balancing status of the n-th cell. 1 = discharge enable.
flags_t gFlags = {0,0,0,0,0,0,0,0,0};    // Les drapeaux globaux utilis�s //TODO: tester l'initialisation

 
/*************************
   Functions prototypes
*************************/

void MCU_init(void);
void checkForBalancedCells(uint16*, uint16[], uint16, uint16);


void main(void) 
{
   uint8 i, txError=0, openWireCell=0;
   uint8 rcvConfig[6];

   //LTC6802 configuration register initialisation
   //---------------------------------------------
   //GPIO pins pull down off
   //Level polling mode           (after polling command, SDO is low when LTC6802 is busy)
   //Ignore cells 11 and 12
   //Comparator duty cycle = 1    (comparator off, value used to verify config integrity, do not change)
   //Discharge cells off
   //No cell interrupts           (not used)
   //Undervoltage comparaison Voltage = 2.688V  (not used)
   //Overvoltage comparaison Voltage = 4.2V     (not used)
   
   Config_Reg_t ltcConfig = {0b11111001,0,0,0,112,175}; //CFG0 default read = 0b01000000


   //Initialization of the uC peripherals
   //--------------------------------------------------------
   //Timers initiazed for
   //    cell temperature measurements every 5 seconds (with uC ADC)
   //    cell voltage measurements every 1 second (with external battery monitor)
   //    we have a precision of 10 bits on the temperature measurements
   //    we have a precision of 12 bits on the voltage measurements
   
	MCU_init();

   //Slave ID reading on port E
   gSlaveID = PORTE & 0x0F;   //Only the 4 lower bits are meaningful
   if(gSlaveID == 0 || (gSlaveID > 10)) {
      gFlags.badSlaveId = 1;     //TODO: we do nothing with this so far
   }
   
   
   //CAN synchronization
   #ifdef CAN_ENABLE
   while(!CAN0CTL0_SYNCH);   // Wait for Synchronization 
   
   //Envoyer un message au ma�tre pour indiquer que notre initialisation est compl�t�e
   //Envoyer le num�ro de r�vision �galement
   gFlags.canTxError = CAN0SendInitStatus(gSlaveID); 
   gFlags.canTxError = CAN0SendFirmwareRevision(gSlaveID);  
   #endif
    
    
	//Timers activation
   PITCE_PCE0 = 1;           //Activation du timer PIT0. Ce timer a une fr�quence de 1 Hz.
                             //Dans l'interruption, on g�re la prise de mesures.
   PITCFLMT_PITE = 1;        //Activation of the timer module
   
   //Program the LTC6802-2 configuration registers   
   ltcWriteConfig(&ltcConfig);
      
	while(1) {
  
      #ifdef CAN_ENABLE
      if(gFlags.firmwareRequest) { 
          gFlags.canTxError = CAN0SendFirmwareRevision(gSlaveID);
          gFlags.firmwareRequest = 0;
      }
      #endif
  
      //Temp measurements are ready to be sent to the master (every 5 seconds)
      if(gFlags.ADC0done && gFlags.ADC1done){

         //On ferme le ADC pour l'�conomie d'�nergie
         ATD0CTL2_ADPU = 0;
         ATD1CTL2_ADPU = 0;
      
         //Conversion of temperatures to a readable form (ex: 295 = 29.5 oC)
         for(i=0; i<NB_CELL; i++)
            gTemp[i] = convertTemp(gTemp[i]);         

         #ifdef CAN_ENABLE
         //send temp to master via CAN
         gFlags.canTxError = CAN0SendTemp(gTemp, gSlaveID);    //TODO: we do nothing with this flag so far...
         #endif

         gFlags.ADC0done = 0;
         gFlags.ADC1done = 0;
      }
      
      
      //Voltages measurements need to be acquired and sent (every second)
      //Open-wire connection detection is done
      //And configuration register verification too
      if(gFlags.voltTimeout){
      
         #ifdef SPI_ENABLE  
         //Send the configuration register if needed.
         while(ltcReadConfig(rcvConfig) != 0);  //Read the current config, without errors.

         if((rcvConfig[0] & 0x01) == 0x00) {//This means that the LTC6802 config register
            gFlags.spiTimeout = 0;                //was reset after its watchdog timed out
            ltcWriteConfig(&ltcConfig);     //We do not use the WTD bit (doesnt work). We use
         }                                  //the CDC bits instead because the default value (0)
                                            //is different than the one we use (1). When the watch dog
                                            //times out, the config registers are reset. The chip is then
                                            //in standby mode, no new measures are taken. A read voltages
                                            //command seems to return the last measured voltages, so no erroneous
                                            //measures are sent to the master.   
         ltcStartVoltageMeasure();
	      txError = ltcReadCellVoltage(gVoltages);
         
         //open-wire connection detection
         openWireCell = ltcVerifyOpenWire();    //TODO: we do nothing with this so far..
         #endif                              
         
         
         #ifdef CAN_ENABLE
         //send voltages to the master via CAN, if received without error from LTC6802
         if(!txError)
            gFlags.canTxError = CAN0SendVoltages(gVoltages, gSlaveID);  //TODO: we do nothing with this flag so far...
         #endif

         gFlags.voltTimeout = 0;
         txError = 0;
      }
      
      //If the config registers have been modified by the equilibration routine,
      //send it to the LTC6802
      if(gFlags.equiStatusChange){
         #ifdef SPI_ENABLE  
         ltcMakeConfigRegister(&ltcConfig, gBalanceVector);
         ltcWriteConfig(&ltcConfig);
         #endif SPI_ENABLE  
         gFlags.equiStatusChange = 0; 
      }
      

      if(gFlags.balancingActive) {          //if in balancing mode
        checkForBalancedCells(&gBalanceVector, gVoltages, gBalThres, BAL_DELTA_VOLT);
        if(!gBalanceVector) {      //all cells are balanced, send the status change to the master
          #ifdef CAN_ENABLE
          gFlags.canTxError = CAN0SendEquiStatus(gBalanceVector, gBalThres, gSlaveID);
          #endif
          gFlags.balancingActive = 0;           
        }
      }
      
      //On entre en mode veille, en attendant le prochain interrupt.
      //Ce sera un timer qui indique le moment de prendre des mesures ou la
      //r�ception d'un message sur le port CAN.
      
      //Le module SPI entrera lui aussi en mode veille puisque SPI1CR2_SPISWAI = 1
      //On envoie l'ADC en stop mode quand il a termin� de prendre ses mesures.
      //Le p�riph�rique CAN n'entre pas en mode wait car dans ce cas on perd le msg
      //qui a r�veill� l'interface CAN.
      
      //TODO: mettre le module CAN en wait mode et tjrs envoyer un msg bidon � partir du ma�tre
      //quand il envoie une commande. Cependant, les mesg des autres esclaves vont constamment
      //r�veiller l'interface, sauf si on utilise le Twup...
      
      asm {
        WAI
      };    
      
	} //end while(1)
    
} //end main

// Functions implementation.
void checkForBalancedCells(uint16* curBalanceArray, uint16 voltages[],
  uint16 targetVoltage, uint16 fuzzFactor) {
  uint8 i = 0;
  for (i = 0; i < NB_CELL; i++) {
    if ((*curBalanceArray & (1 << i)) &&  // If this cell is currently discharging
        (voltages[i] < (targetVoltage + fuzzFactor))) {  // And it is equilibrated
      *curBalanceArray &= ~(1 << i);  // Remove it's equilibration bit.
      gFlags.equiStatusChange = 1;  // Ask to update register.
    }
  }
}