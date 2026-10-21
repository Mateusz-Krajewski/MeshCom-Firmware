/*
This file contains all web-based remotely callable functions
*/
#include "web_nodefunctioncalls.h"
#include <command_functions.h>
#include <loop_functions.h>
#include <loop_functions_extern.h>
#include "web_task.h"
#include <string> 



void webFunctionCall(funCallStruct* functionData) {
    bool bPhoneReady = (isPhoneReady == 1);
    (void)bPhoneReady;

    //Serial.printf("##### Executing: %s with param: %s\n", functionData->functionName, functionData->functionParameter);
    if(functionData->functionName.compareTo("sendpos")==0) {
        functionData->returnCode = WF_RETURNCODE_OKAY;
        if(bDisplayTrack) {
            if(!webSubmitCommand((char*)"--sendtrack"))
                functionData->returnCode = WF_RETURNCODE_FAIL;
            //Serial.println("SendTrack");
        } else {
            if(!webSubmitCommand((char*)"--sendpos"))
                functionData->returnCode = WF_RETURNCODE_FAIL;
            //Serial.println("SendPos");
        }
        return;
    } else
    if(functionData->functionName.compareTo("reboot")==0) {
                if(!webSubmitCommand((char*)"--reboot"))
                    functionData->returnCode = WF_RETURNCODE_FAIL;
                else
                    functionData->returnCode = WF_RETURNCODE_OKAY;
                return;
    }
    #ifdef ESP32
    if(functionData->functionName.compareTo("otaupdate")==0) {
                if(!webSubmitCommand((char*)"--ota-update"))
                    functionData->returnCode = WF_RETURNCODE_FAIL;
                else
                    functionData->returnCode = WF_RETURNCODE_OKAY;
                return;
    }
    #endif


    //if nothiung matched, then the function is not known.
    functionData->returnCode = WF_RETURNCODE_FAIL;
    return;
}
