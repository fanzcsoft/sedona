#ifdef __DARWIN__
#include <sys/types.h>
#endif

#include <stdio.h>
#include "MQTTClient.h"
#include "communityMQTT_common.h"

#include <stdlib.h>
#include <signal.h>
#include <memory.h>
#include <sys/time.h>

#include <pthread.h>
#include "uthash.h"

#include "sedona.h"

static pthread_key_t thread_key;
static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;

///////////////////////////////////////////////////////
// Worker Thread Functions
///////////////////////////////////////////////////////

static void makeThreadKey()
{
  (void)pthread_key_create(&thread_key, NULL);
}

void releaseSession(SessionHandle * pSession)
{
  if (!pSession)
    return;
  
  //free all payloads
  while (curPayload(pSession))
    popPayload(pSession);

  MQTTHandle * pHandle = pSession->pHandle;
  if (pHandle)
  {
    free(pHandle->pClient);
    free(pHandle->pNetwork);
    free(pHandle->buf);
    free(pHandle->readbuf);
    free(pHandle);
  }
  
  SubscribeResponse * pResponse = pSession->pResponse;
  if (pResponse)
  {
    SubscribeResponse * current = NULL;
    SubscribeResponse * tmp = NULL;
    HASH_ITER(hh, pResponse, current, tmp) {
      HASH_DEL(pResponse, current);
      if (current->topic)
        free(current->topic);
      if (current->payload)
        free(current->payload);
      free(current);
    }
  }

  free(pSession);
}

bool startSession(SessionHandle * pSession, StartSessionData * pData) 
{
  if (!pSession)
    return false;
  
  if (!pSession->pHandle)
    pSession->pHandle = malloc(sizeof(MQTTHandle));
  MQTTHandle * pHandle = pSession->pHandle;
  pHandle->buf_len = 100;
  pHandle->buf = malloc(sizeof(char)*pHandle->buf_len);
  pHandle->readbuf_len = 100;
  pHandle->readbuf = malloc(sizeof(char)*pHandle->readbuf_len);

  pHandle->pNetwork = malloc(sizeof(Network));
  pHandle->pClient = malloc(sizeof(Client));
  /* pHandle->command_timeout_ms = 1000; */
  pHandle->command_timeout_ms = 3000;

  NewNetwork(pHandle->pNetwork);
  MQTTClient(pHandle->pClient, pHandle->pNetwork, pHandle->command_timeout_ms, pHandle->buf, pHandle->buf_len, pHandle->readbuf, pHandle->readbuf_len);

  int rc = 0;
  rc = ConnectNetwork(pHandle->pNetwork, pData->host, pData->port);
  if (rc != SUCCESS)
    return false;
 
  MQTTPacket_connectData data = MQTTPacket_connectData_initializer;       
  data.willFlag = 0;
  data.MQTTVersion = 3;
  data.clientID.cstring = pData->clientid;
  data.username.cstring = pData->username;
  data.password.cstring = pData->password;

  data.keepAliveInterval = 10;
  data.cleansession = 1;
  printf(" * [MQTTService] Connecting to %s:%d\n", pData->host, pData->port);
  
  rc = 0;
  rc = MQTTConnect(pHandle->pClient, &data);
  /* printf(" * [MQTTService] Connected %d\n", rc); */
  return rc == SUCCESS;
}

bool publish(SessionHandle * pSession, PublishData * pData)
{
  if (!pSession)
    return false;

  MQTTHandle * pHandle = pSession->pHandle;
  if (!pHandle || !pHandle->pClient)
  {
    printf(" * [MQTTService] Invalid Handle");
    return false;
  }
  if (!pHandle->pClient->isconnected)
  {
    printf(" * [MQTTService] Connection lost");
    return false;
  }

  /* printf(" * [MQTTService] Publish to %s\n", pData->topic); */
  MQTTMessage msg;
  msg.qos = pData->qos;
  msg.retained = 0;
  msg.dup = 0;
  msg.payload = pData->payload;
  msg.payloadlen = pData->payload_len;
  int rc = MQTTPublish(pHandle->pClient, pData->topic, &msg);
  /* printf(" * [MQTTService] Published %d\n", rc); */
  return rc == SUCCESS;
}

void messageArrived(MessageData * pMsgData)
{
  MQTTMessage* message = pMsgData->message;
  if (!message)
    return;
  
  SessionHandle * pSession = (SessionHandle *)pthread_getspecific(thread_key);
  if (!pSession)
    return;
  
  SubscribeResponse * pResponse = NULL;

  char * topic = malloc(pMsgData->topicName->lenstring.len+1);
  memset(topic, 0, pMsgData->topicName->lenstring.len+1);
  strncpy(topic, pMsgData->topicName->lenstring.data, pMsgData->topicName->lenstring.len);
  HASH_FIND_STR(pSession->pResponse, topic, pResponse);
  if (!pResponse)
  {
    pResponse = malloc(sizeof(SubscribeResponse));
    memset(pResponse, 0, sizeof(SubscribeResponse));

    pResponse->topic = topic;
    HASH_ADD_STR(pSession->pResponse, topic, pResponse);
  }
  else
    free(topic);

  if (pResponse->payload)
    free(pResponse->payload);
  pResponse->payload = malloc(pMsgData->message->payloadlen);
  memset(pResponse->payload, 0, pMsgData->message->payloadlen);
  memcpy(pResponse->payload, pMsgData->message->payload, pMsgData->message->payloadlen);
  pResponse->payload_len = pMsgData->message->payloadlen;
}

bool subscribe(SessionHandle * pSession, SubscribeData * pData)
{
  if (!pSession)
    return false;

  MQTTHandle * pHandle = pSession->pHandle;
  if (!pHandle || !pHandle->pClient)
  {
    printf(" * [MQTTService] Invalid Handle");
    return false;
  }
  if (!pHandle->pClient->isconnected)
  {
    printf(" * [MQTTService] Connection lost");
    return false;
  }

  printf(" * [MQTTService] Subscribe to '%s'(qos: %d)\n", pData->topic, pData->qos);

  //TODO: support wildcard in topic, refer to 'deliverMessage' method
  int rc = MQTTSubscribe(pHandle->pClient, pData->topic, pData->qos, messageArrived);
  return rc == SUCCESS;
}

bool unsubscribe(SessionHandle * pSession, UnsubscribeData * pData)
{
  if (!pSession)
    return false;

  MQTTHandle * pHandle = pSession->pHandle;
  if (!pHandle || !pHandle->pClient)
  {
    printf(" * [MQTTService] Invalid Handle");
    return false;
  }
  if (!pHandle->pClient->isconnected)
  {
    printf(" * [MQTTService] Connection lost");
    return false;
  }
  
  printf(" * [MQTTService] Unsubscribe to '%s'\n", pData->topic);

  int rc = MQTTUnsubscribe(pHandle->pClient, pData->topic);
  return rc == SUCCESS;
}

bool yield(MQTTHandle * pHandle)
{
  if (!pHandle || !pHandle->pClient)
  {
    printf(" * [MQTTService] Invalid MQTTHandle\n");
    return false;
  }
  if (!pHandle->pClient->isconnected)
    return false;

  int rc = MQTTYield(pHandle->pClient, 1000); 
  return rc == SUCCESS;
}

bool stopSession(SessionHandle * pSession)
{
  if (!pSession)
    return false;

  MQTTHandle * pHandle = pSession->pHandle;
  if (!pHandle || !pHandle->pClient || !pHandle->pNetwork) 
  {
    printf(" * [MQTTService] Invalid MQTTHandle\n");
    return false;
  }

  if (pHandle->pClient->isconnected)
    MQTTDisconnect(pHandle->pClient);
  pHandle->pNetwork->disconnect(pHandle->pNetwork);
  releaseSession(pSession);
  return true;
}

void * workerThreadFunc(void * pThreadData)
{
  SessionHandle * pSession = (SessionHandle *)pThreadData;
  if (!pSession)
    return NULL;
  
  pthread_setspecific(thread_key, pSession);

  printf(" * [MQTTService] MQTT worker thread started \n");
  while (true) 
  {
    Payload * pPayload = curPayload(pSession);
    if (!pPayload)
    {
      if (pSession->pHandle)
      {
        yield(pSession->pHandle);
      }
      continue;
    }
    else
    {
      bool result = true;
      switch (pPayload->type)
      {
        case StartSessionTask:
          result = startSession(pSession, pPayload->pStartSessionData);
          break;
        case PublishTask:
          result = publish(pSession, pPayload->pPublishData);
          break;
        case SubscribeTask:
          result = subscribe(pSession, pPayload->pSubscribeData);
          break;
        case UnsubscribeTask:
          result = unsubscribe(pSession, pPayload->pUnsubscribeData);
          break;
        case StopSessionTask:
          stopSession(pSession);
          pSession = NULL;
          pthread_setspecific(thread_key, pSession);
          printf(" * [MQTTService] MQTT worker thread exited \n");
          pthread_exit(NULL);
          break;
        default:
          break;
      }
      /* printf("###  * %d action result: %d \n", pPayload->type, result); */
      popPayload(pSession);
    }
  }   
  return NULL;
}

///////////////////////////////////////////////////////
// Native Method Slots
///////////////////////////////////////////////////////
Cell communityMQTT_Worker_startSession(SedonaVM* vm, Cell* params)
{
  char * host = params[0].aval;
  int32_t port = params[1].ival;
  char * clientid = params[2].aval;
  char * username = params[3].aval;
  char * password = params[4].aval;
  
  pthread_once(&thread_key_once, makeThreadKey);
  
  SessionHandle * pSession = malloc(sizeof(SessionHandle));
  pSession->pHandle = NULL;
  pSession->pHead = NULL;

  StartSessionData * pData = malloc(sizeof(StartSessionData));
  pData->host = malloc(strlen(host)+1);
  strcpy(pData->host, host);
  pData->port = port;
  pData->clientid = malloc(strlen(clientid)+1);
  strcpy(pData->clientid, clientid);
  pData->username = malloc(strlen(username)+1);
  strcpy(pData->username, username);
  pData->password = malloc(strlen(password)+1);
  strcpy(pData->password, password);

  Payload * pPayload = malloc(sizeof(Payload));
  pPayload->type = StartSessionTask;
  pPayload->pStartSessionData = pData;

  pushPayload(pSession, pPayload); 

  pthread_t * pthread = malloc(sizeof(pthread_t));
  int rc = pthread_create(pthread, NULL, &workerThreadFunc, pSession);
  if (rc)
  {
    printf(" * [MQTTService] Thread creation failed: %d\n", rc);
    free(pthread);
    pthread = NULL;
    
    releaseSession(pSession);
    pSession = NULL;
  }
  else
    pthread_detach(*pthread); //reclaim thread resource after it terminates

  Cell result;
  result.aval = (void *)pSession;
  return result;
}

Cell communityMQTT_Worker_stopSession(SedonaVM* vm, Cell* params)
{
  SessionHandle * pSession = (SessionHandle *)params[0].aval;
  Payload * pPayload = malloc(sizeof(Payload));
  pPayload->type = StopSessionTask;
  pushPayload(pSession, pPayload);

  return nullCell;
}

Cell communityMQTT_Worker_isSessionLive(SedonaVM* vm, Cell* params)
{
  SessionHandle * pSession = (SessionHandle *)params[0].aval;
  if (!pSession)
    return falseCell;

  MQTTHandle * pHandle = pSession->pHandle;
  if (pHandle && pHandle->pClient && pHandle->pClient->isconnected)
    return trueCell;
  else
    return falseCell;
}
