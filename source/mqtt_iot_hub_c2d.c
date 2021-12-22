/******************************************************************************
* File Name: mqtt_iot_hub_c2d.c
*
* Description: This file contains tasks and functions related to Azure C2D
* feature task.
*
********************************************************************************
* Copyright 2021, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

#include "azure_common.h"

#include <stdio.h>
#include "cyhal.h"
#include "cybsp.h"
#include "cy_log.h"
#include <lwip/tcpip.h>
#include <lwip/api.h>
#include <cy_retarget_io.h>
#include "cy_lwip.h"
#include <cybsp_wifi.h>
#include "cy_nw_helper.h"
#include "cy_wcm.h"
#include "cyabs_rtos.h"

#include <FreeRTOS.h>
#include <task.h>

#include "mqtt_main.h"
#include "cy_mqtt_api.h"
#include <az_core.h>
#include <az_iot.h>
#include "mqtt_iot_common.h"

#ifdef CY_TFM_PSA_SUPPORTED
#include "cy_wdt.h"
#include "tfm_multi_core_api.h"
#include "tfm_ns_interface.h"
#include "tfm_ns_mailbox.h"
#include "psa/protected_storage.h"
#endif

/*******************************************************************************
* Macros
********************************************************************************/
/* Wait duration between messages, should be in multiples of 10 */
#define MESSAGE_WAIT_LOOP_DURATION_SEC              (120)

#define MESSAGE_WAIT_DELAY_MSEC                     (1000 * 10)

#define SAS_KEY_DURATION_MINUTES                    (240)

#define MQTT_CLIENT_ID_BUFFER_SIZE                  (128)

/***********************************************************
* Static Variables
************************************************************/
static cy_mqtt_t                           mqtthandle;
static iot_sample_environment_variables    env_vars;
static az_iot_hub_client                   hub_client;
static char                                mqtt_client_username_buffer[IOT_SAMPLE_APP_BUFFER_SIZE_IN_BYTES];
static char                                mqtt_endpoint_buffer[IOT_SAMPLE_APP_BUFFER_SIZE_IN_BYTES];
static volatile bool                       connect_state = false;

#if SAS_TOKEN_AUTH
static iot_sample_credentials              sas_credentials;
static char                                device_id_buffer[IOT_SAMPLE_APP_BUFFER_SIZE_IN_BYTES];
static char                                sas_token_buffer[IOT_SAMPLE_APP_BUFFER_SIZE_IN_BYTES];
#endif

/* The network buffer must remain valid for the lifetime of the MQTT context. */
static uint8_t                             *buffer = NULL;

static void parse_c2d_message( char* topic, uint16_t topic_len,  cy_mqtt_publish_info_t *message,
        az_iot_hub_client_c2d_request* out_c2d_request )
{
    az_span const topic_span = az_span_create( (uint8_t*)topic, topic_len );
    az_span const message_span = az_span_create( (uint8_t*)message->payload, message->payload_len );

    /* Parse the message and retrieve the c2d_request information */
    az_result rc = az_iot_hub_client_c2d_parse_received_topic( &hub_client, topic_span, out_c2d_request );
    if( az_result_failed(rc) )
    {
        TEST_INFO(( "\r\nMessage from unknown topic: az_result return code 0x%08x.", (unsigned int)rc ));
        TEST_INFO(( "\r\nTopic : %.*s\r\n", (int)topic_span._internal.size, topic_span._internal.ptr ));
        return;
    }

    TEST_INFO(( "\r\nClient received a valid topic response." ));
    TEST_INFO(( "\r\nTopic : %.*s\r\n", (int)topic_span._internal.size, topic_span._internal.ptr ));
    TEST_INFO(( "\r\nPayload: %.*s\r\n", (int)message_span._internal.size,  message_span._internal.ptr ));
}

static void mqtt_event_cb( cy_mqtt_t mqtt_handle, cy_mqtt_event_t event, void *arg )
{
    cy_mqtt_publish_info_t *received_msg;
    az_iot_hub_client_c2d_request c2d_request;

    TEST_INFO(( "\r\nMQTT App callback with handle : %p \n", mqtt_handle ));

    switch( event.type )
    {
        case CY_MQTT_EVENT_TYPE_DISCONNECT :
            if( event.data.reason == CY_MQTT_DISCONN_TYPE_BROKER_DOWN )
            {
                TEST_INFO(( "\r\nCY_MQTT_DISCONN_TYPE_BROKER_DOWN .....\n" ));
            }
            else
            {
                TEST_INFO(( "\r\nCY_MQTT_DISCONN_REASON_NETWORK_DISCONNECTION .....\n" ));
            }
            connect_state = false;
            break;

        case CY_MQTT_EVENT_TYPE_PUBLISH_RECEIVE :
            received_msg = &(event.data.pub_msg.received_message);
            parse_c2d_message( (char*)received_msg->topic, received_msg->topic_len, received_msg , &c2d_request );
            TEST_INFO(( "\r\nClient parsed C2D message." ));
            break;

        default :
            TEST_INFO(( "\r\nUnknown Event .....\n" ));
            break;
    }
}

static cy_rslt_t disconnect_and_delete_mqtt_client( void )
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    result = cy_mqtt_disconnect( mqtthandle );
    if( result == CY_RSLT_SUCCESS )
    {
        TEST_INFO(( "\r\ncy_mqtt_disconnect ----------------------- Pass \n" ));
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_disconnect ----------------------- Fail \n" ));
    }
    connect_state = false;

    result = cy_mqtt_delete( mqtthandle );
    if( result == TEST_PASS )
    {
        TEST_INFO(( "\r\ncy_mqtt_delete --------------------------- Pass \n" ));
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_delete --------------------------- Fail \n" ));
    }

    result = cy_mqtt_deinit();
    if( result == TEST_PASS )
    {
        TEST_INFO(( "\r\ncy_mqtt_deinit --------------------------- Pass \n" ));
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_deinit --------------------------- Fail \n" ));
    }

    /* Free the network buffer */
    if( buffer != NULL )
    {
        free( buffer );
        buffer = NULL;
    }

    return result;
}

static cy_rslt_t subscribe_mqtt_client_to_iot_hub_topics( void )
{
    cy_rslt_t result = CY_RSLT_SUCCESS;
    cy_mqtt_subscribe_info_t sub_msg;

    sub_msg.qos = (cy_mqtt_qos_t)CY_MQTT_QOS1;
    sub_msg.topic = AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC;
    sub_msg.topic_len = ( ( uint16_t ) ( sizeof( AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC ) - 1 ) );

    result = cy_mqtt_subscribe( mqtthandle, &sub_msg, 1 );
    if( result == TEST_PASS )
    {
        TEST_INFO(( "\r\ncy_mqtt_subscribe ------------------------ Pass \r\n" ));
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_subscribe ------------------------ Fail \r\n" ));
    }

    return result;
}

static cy_rslt_t connect_mqtt_client_to_iot_hub( void )
{
    cy_rslt_t result = CY_RSLT_SUCCESS;
    int rc;
    size_t username_len = 0, client_id_len = 0;
    cy_mqtt_connect_info_t connect_info;

    /* Get the MQTT client ID used for the MQTT connection */
    char mqtt_client_id_buffer[MQTT_CLIENT_ID_BUFFER_SIZE];

    memset( &connect_info, 0x00, sizeof( cy_mqtt_connect_info_t ) );
    memset( &mqtt_client_id_buffer, 0x00, sizeof( mqtt_client_id_buffer ) );

    rc = az_iot_hub_client_get_client_id( &hub_client, mqtt_client_id_buffer, sizeof(mqtt_client_id_buffer), &client_id_len );
    if( az_result_failed(rc) )
    {
        TEST_INFO(( "\r\nFailed to get MQTT client id: az_result return code 0x%08x.", rc ));
        return TEST_FAIL;
    }

    /* Get the MQTT client user name */
    rc = az_iot_hub_client_get_user_name( &hub_client, mqtt_client_username_buffer,
            sizeof(mqtt_client_username_buffer), &username_len );
    if( az_result_failed(rc) )
    {
        TEST_INFO(( "\r\nFailed to get MQTT client username: az_result return code 0x%08x.", rc ));
        return TEST_FAIL;
    }

    connect_info.client_id = mqtt_client_id_buffer;
    connect_info.client_id_len = client_id_len;
    connect_info.keep_alive_sec = MQTT_KEEP_ALIVE_INTERVAL_SECONDS;
    connect_info.clean_session = false;
    connect_info.will_info = NULL;

#if SAS_TOKEN_AUTH
    connect_info.username = mqtt_client_username_buffer;
    connect_info.password = (char *)sas_credentials.sas_token;
    connect_info.username_len = username_len;
    connect_info.password_len = sas_credentials.sas_token_len;
#else
    connect_info.username = mqtt_client_username_buffer;
    connect_info.password = NULL;
    connect_info.username_len = username_len;
    connect_info.password_len = 0;
#endif

    result = cy_mqtt_connect( mqtthandle, &connect_info );
    if( result == TEST_PASS )
    {
        TEST_INFO(("\r\ncy_mqtt_connect -------------------------- Pass \n"));
        connect_state = true;
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_connect -------------------------- Fail \n" ));
        return TEST_FAIL;
    }

    return CY_RSLT_SUCCESS;
}

static cy_rslt_t create_and_configure_mqtt_client( void )
{
    int32_t rc;
    uint16_t ep_size = 0;
    cy_rslt_t result = TEST_PASS;
    cy_awsport_ssl_credentials_t credentials;
    cy_awsport_ssl_credentials_t *security = NULL;
    cy_mqtt_broker_info_t broker_info;

    memset( &mqtt_endpoint_buffer, 0x00, sizeof( mqtt_endpoint_buffer ) );

    ep_size = iot_sample_create_mqtt_endpoint( CY_MQTT_IOT_HUB, &env_vars,
            mqtt_endpoint_buffer,
            sizeof(mqtt_endpoint_buffer) );

    /* Initialize the hub client with the default connection options */
    rc = az_iot_hub_client_init( &hub_client, env_vars.hub_hostname, env_vars.hub_device_id, NULL );
    if( az_result_failed( rc ) )
    {
        TEST_INFO(( "\r\nFailed to initialize hub client: az_result return code 0x%08x.", (unsigned int)rc ));
        return TEST_FAIL;
    }

    /* Allocate the network buffer */
    buffer = (uint8_t *) malloc( sizeof(uint8_t) * NETWORK_BUFFER_SIZE );
    if( buffer == NULL )
    {
        TEST_INFO(( "\r\nNo memory is available for network buffer..! \n" ));
        return TEST_FAIL;
    }

    memset( &broker_info, 0x00, sizeof( cy_mqtt_broker_info_t ) );
    memset( &credentials, 0x00, sizeof( cy_awsport_ssl_credentials_t ) );

    result = cy_mqtt_init();
    if( result == TEST_PASS )
    {
        TEST_INFO(( "\r\ncy_mqtt_init ----------------------------- Pass \n" ));
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_init ----------------------------- Fail \n" ));
        return TEST_FAIL;
    }

#if SAS_TOKEN_AUTH
    credentials.client_cert = (const char *) NULL;
    credentials.client_cert_size = 0;
    credentials.private_key = (const char *) NULL;
    credentials.private_key_size = 0;
    credentials.root_ca = (const char *) NULL;
    credentials.root_ca_size = 0;
    /* For SAS token-based authentication mode, RootCA verification is not
     * required */
    credentials.root_ca_verify_mode = CY_AWS_ROOTCA_VERIFY_NONE;
    /* Set certificate and key location */
    credentials.cert_key_location = CY_AWS_CERT_KEY_LOCATION_RAM;
    credentials.root_ca_location = CY_AWS_CERT_KEY_LOCATION_RAM;
#else
    credentials.client_cert = (const char *) &azure_client_cert;
    credentials.client_cert_size = IOT_AZURE_CLIENT_CERT_LENGTH;
    credentials.private_key = (const char *) &azure_client_key;
    credentials.private_key_size = IOT_AZURE_CLIENT_KEY_LENGTH;
    credentials.root_ca = (const char *) &azure_root_ca_certificate;
    credentials.root_ca_size = IOT_AZURE_ROOT_CA_LENGTH;
#endif
    broker_info.hostname = (const char *)&mqtt_endpoint_buffer;
    broker_info.hostname_len = (uint16_t)ep_size;
    broker_info.port = IOT_DEMO_PORT_AZURE_S;
    security = &credentials;

    result = cy_mqtt_create( buffer, NETWORK_BUFFER_SIZE,
            security, &broker_info,
            (cy_mqtt_callback_t)mqtt_event_cb, NULL,
            &mqtthandle );
    if( result == TEST_PASS )
    {
        TEST_INFO(( "\r\ncy_mqtt_create ----------------------------- Pass \n" ));
    }
    else
    {
        TEST_INFO(( "\r\ncy_mqtt_create ----------------------------- Fail \n" ));
        return TEST_FAIL;
    }

    return CY_RSLT_SUCCESS;
}

static cy_rslt_t configure_hub_environment_variables( void )
{
    cy_rslt_t result = TEST_PASS;
#if SAS_TOKEN_AUTH
    env_vars.hub_device_id._internal.ptr = (uint8_t*) sas_credentials.device_id;
    env_vars.hub_device_id._internal.size = (int32_t) sas_credentials.device_id_len;
    env_vars.hub_sas_key._internal.ptr = (uint8_t*) sas_credentials.sas_token;
    env_vars.hub_sas_key._internal.size = (int32_t) sas_credentials.sas_token_len;
#else
    env_vars.hub_device_id._internal.ptr = (uint8_t*) MQTT_CLIENT_IDENTIFIER_AZURE_CERT;
    env_vars.hub_device_id._internal.size = MQTT_CLIENT_IDENTIFIER_AZURE_CERT_LENGTH;
    env_vars.hub_sas_key._internal.ptr = NULL;
    env_vars.hub_sas_key._internal.size = 0;
#endif
    env_vars.hub_hostname._internal.ptr = (uint8_t*) IOT_DEMO_SERVER_AZURE;
    env_vars.hub_hostname._internal.size = strlen(IOT_DEMO_SERVER_AZURE);
    env_vars.sas_key_duration_minutes = SAS_KEY_DURATION_MINUTES;
    return result;
}

void Azure_hub_c2d_app(void *arg)
{
    cy_rslt_t TestRes = TEST_PASS ;
    uint8_t Failcount = 0, Passcount = 0, time_sec = 0;
#ifdef CY_TFM_PSA_SUPPORTED
    psa_status_t uxStatus = PSA_SUCCESS;
    size_t read_len = 0;
    (void)uxStatus;
    (void)read_len;
#endif

    cy_log_init( CY_LOG_ERR, NULL, NULL );

#if SAS_TOKEN_AUTH
    (void)device_id_buffer;
    (void)sas_token_buffer;
#if ( (defined CY_TFM_PSA_SUPPORTED) && ( SAS_TOKEN_LOCATION_FLASH == false ) )
    /* Read the Device ID from the secured memory */
    uxStatus = psa_ps_get( PSA_DEVICEID_UID, 0, sizeof(device_id_buffer), device_id_buffer, &read_len );
    if( uxStatus == PSA_SUCCESS )
    {
        device_id_buffer[read_len] = '\0';
        TEST_INFO(( "\r\nRetrieved Device ID : %s\r\n", device_id_buffer ));
        sas_credentials.device_id = (uint8_t*)device_id_buffer;
        sas_credentials.device_id_len = read_len;
    }
    else
    {
        TEST_INFO(( "\r\npsa_ps_get for Device ID failed with %d\n", (int)uxStatus ));
        TEST_INFO(( "\r\nTaken Device ID from MQTT_CLIENT_IDENTIFIER_AZURE_SAS macro. \n" ));
        sas_credentials.device_id = (uint8_t*)MQTT_CLIENT_IDENTIFIER_AZURE_SAS;
        sas_credentials.device_id_len = MQTT_CLIENT_IDENTIFIER_AZURE_SAS_LENGTH;
    }

    read_len = 0;

    /* Read the SAS token from the secured memory */
    uxStatus = psa_ps_get( PSA_SAS_TOKEN_UID, 0, sizeof(sas_token_buffer), sas_token_buffer, &read_len );
    if( uxStatus == PSA_SUCCESS )
    {
        sas_token_buffer[read_len] = '\0';
        TEST_INFO(( "\r\nRetrieved SAS token : %s\r\n", sas_token_buffer ));
        sas_credentials.sas_token = (uint8_t*)sas_token_buffer;
        sas_credentials.sas_token_len = read_len;
    }
    else
    {
        TEST_INFO(( "\r\npsa_ps_get for sas_token failed with %d\n", (int)uxStatus ));
        TEST_INFO(( "\r\nTaken SAS Token from IOT_AZURE_PASSWORD macro. \n" ));
        sas_credentials.sas_token = (uint8_t *)IOT_AZURE_PASSWORD;
        sas_credentials.sas_token_len = IOT_AZURE_PASSWORD_LENGTH;
    }
#else
    sas_credentials.device_id = (uint8_t*) MQTT_CLIENT_IDENTIFIER_AZURE_SAS;
    sas_credentials.device_id_len = MQTT_CLIENT_IDENTIFIER_AZURE_SAS_LENGTH;
    sas_credentials.sas_token = (uint8_t*) IOT_AZURE_PASSWORD;
    sas_credentials.sas_token_len = IOT_AZURE_PASSWORD_LENGTH;
#endif

#endif /* SAS_TOKEN_AUTH */

    TestRes = configure_hub_environment_variables();
    if( TestRes == TEST_PASS )
    {
        TEST_INFO(( "\r\nconfigure_environment_variables ----------- Pass \r\n" ));
        Passcount++;
    }
    else
    {
        TEST_INFO(( "\r\nconfigure_environment_variables ----------- Fail \r\n" ));
        Failcount++;
        goto exit;
    }

    TestRes = create_and_configure_mqtt_client();
    if( TestRes == TEST_PASS )
    {
        TEST_INFO(( "\r\ncreate_and_configure_mqtt_client ----------- Pass \r\n" ));
        Passcount++;
    }
    else
    {
        TEST_INFO(( "\r\ncreate_and_configure_mqtt_client ----------- Fail \r\n" ));
        Failcount++;
        goto exit;
    }

    TestRes = connect_mqtt_client_to_iot_hub();
    if( TestRes == TEST_PASS )
    {
        TEST_INFO(( "\r\nconnect_mqtt_client_to_iot_hub ----------- Pass \r\n" ));
        Passcount++;
    }
    else
    {
        TEST_INFO(( "\r\nconnect_mqtt_client_to_iot_hub ----------- Fail \r\n" ));
        Failcount++;
        goto exit;
    }

    TestRes = subscribe_mqtt_client_to_iot_hub_topics();
    if( TestRes == TEST_PASS )
    {
        TEST_INFO(( "\r\nsubscribe_mqtt_client_to_iot_hub_topics ----------- Pass \r\n" ));
        Passcount++;
    }
    else
    {
        TEST_INFO(( "\r\nsubscribe_mqtt_client_to_iot_hub_topics ----------- Fail \r\n" ));
        Failcount++;
    }

    /* 2 Min check to make the app to work with CI/CD */
    time_sec = MESSAGE_WAIT_LOOP_DURATION_SEC;
    while( (connect_state) && (time_sec > 0) )
    {
        TEST_INFO(( "\r\nWaiting for cloud to device messages...... \r\n" ));
        vTaskDelay( MESSAGE_WAIT_DELAY_MSEC );
        time_sec = time_sec - 10;
    }

    TEST_INFO(( "\r\nDisconnecting from broker...... \r\n" ));
    TestRes = disconnect_and_delete_mqtt_client();
    if( TestRes == TEST_PASS )
    {
        TEST_INFO(( "\r\ndisconnect_and_delete_mqtt_client ----------- Pass \r\n" ));
        Passcount++;
    }
    else
    {
        TEST_INFO(( "\r\ndisconnect_and_delete_mqtt_client ----------- Fail \r\n" ));
        Failcount++;
    }

exit:
    TEST_INFO(( "\r\nCompleted MQTT Client Test Cases --------------------------\r\n" ));
    TEST_INFO(( "\r\nTotal Test Cases   ---------------------- %d\r\n", ( Failcount + Passcount ) ));
    TEST_INFO(( "\r\nTest Cases passed  ---------------------- %d\r\n", Passcount ));
    TEST_INFO(( "\r\nTest Cases failed  ---------------------- %d\r\n", Failcount ));

    vTaskSuspend(NULL);

}

/* [] END OF FILE */
