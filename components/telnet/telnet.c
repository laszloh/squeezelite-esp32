	/**
 * Test the telnet functions.
 *
 * Perform a test using the telnet functions.
 * This code exports two new global functions:
 *
 * void telnet_listenForClients(void (*callback)(uint8_t *buffer, size_t size))
 * void telnet_sendData(uint8_t *buffer, size_t size)
 *
 * For additional details and documentation see:
 * * Free book on ESP32 - https://leanpub.com/kolban-ESP32
 *
 *
 * Neil Kolban <kolban1@kolban.com>
 *
 * ****************************
 * Additional portions were taken from
 * https://github.com/PocketSprite/8bkc-sdk/blob/master/8bkc-components/8bkc-hal/vfs-stdout.c
 *
 */
#include <stdlib.h> // Required for libtelnet.h
#include <esp_log.h>
#include "libtelnet.h"
#include "stdbool.h"
#include <lwip/def.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/ringbuf.h"
#include "esp_app_trace.h"
#include "telnet.h"
#include "esp_vfs.h"
#include "esp_vfs_dev.h"
#include "esp_attr.h"
#include "soc/uart_struct.h"
#include "driver/uart.h"
#include "config.h"
#include "nvs_utilities.h"
#include "platform_esp32.h"
#include "trace.h"

/************************************
 * Globals
 */

#define TELNET_STACK_SIZE 8048
#define TELNET_RX_BUF 1024

const static char tag[] = "telnet";
static int uart_fd=0;
RingbufHandle_t buf_handle;
SemaphoreHandle_t xSemaphore = NULL;
static size_t send_chunk=300;
static size_t log_buf_size=2000;      //32-bit aligned size
static bool bIsEnabled=false;
static int partnerSocket=0;
static telnet_t *tnHandle;

/************************************
 * Forward declarations
 */
static void telnet_task(void *data);
static ssize_t stdout_read(int fd, void* data, size_t size);
static int stdout_open(const char * path, int flags, int mode);
static int stdout_close(int fd);
static int stdout_fstat(int fd, struct stat * st);
static ssize_t stdout_write(int fd, const void * data, size_t size);
static char *eventToString(telnet_event_type_t type);
static void handle_telnet_conn();
static void process_logs( UBaseType_t bytes);
static bool bMirrorToUART=false;
struct telnetUserData {
	int sockfd;
	telnet_t *tnHandle;
	char * rxbuf;
};


void init_telnet(){
	char *val= get_nvs_value_alloc(NVS_TYPE_STR, "telnet_enable");
	if (!val || strlen(val) == 0 || !strcasestr("YXD",val) ) {
		ESP_LOGI(tag,"Telnet support disabled");
		if(val) free(val);
		return;
	}
	bMirrorToUART = strcasestr("D",val)!=NULL;

	FREE_AND_NULL(val);
	val=get_nvs_value_alloc(NVS_TYPE_STR, "telnet_block");
	if(val){
		send_chunk=atol(val);
		free(val);
		send_chunk=send_chunk>0?send_chunk:500;
	}
	val=get_nvs_value_alloc(NVS_TYPE_STR, "telnet_buffer");
	if(val){
		log_buf_size=atol(val);
		free(val);
		log_buf_size=log_buf_size>0?log_buf_size:4000;
	}
	// Create the semaphore to guard a shared resource.
	vSemaphoreCreateBinary( xSemaphore );

	// Redirect the output to our telnet handler as soon as possible
	StaticRingbuffer_t *buffer_struct = (StaticRingbuffer_t *)heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	uint8_t *buffer_storage = (uint8_t *)heap_caps_malloc(sizeof(uint8_t)*log_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	buf_handle = xRingbufferCreateStatic(log_buf_size, RINGBUF_TYPE_BYTEBUF, buffer_storage, buffer_struct);
	if (buf_handle == NULL) {
		ESP_LOGE(tag,"Failed to create ring buffer for telnet!");
		return;
	}

	ESP_LOGI(tag, "***Redirecting log output to telnet");
	const esp_vfs_t vfs = {
			.flags = ESP_VFS_FLAG_DEFAULT,
			.write = &stdout_write,
			.open = &stdout_open,
			.fstat = &stdout_fstat,
			.close = &stdout_close,
			.read = &stdout_read,
		};
	if(bMirrorToUART){
		uart_fd=open("/dev/uart/0", O_RDWR);
	}
	ESP_ERROR_CHECK(esp_vfs_register("/dev/pkspstdout", &vfs, NULL));
	freopen("/dev/pkspstdout", "w", stdout);
	freopen("/dev/pkspstdout", "w", stderr);
	bIsEnabled=true;
}
void start_telnet(void * pvParameter){
	static bool isStarted=false;
	StaticTask_t *xTaskBuffer = (StaticTask_t*) heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	StackType_t *xStack = malloc(TELNET_STACK_SIZE);
	
	if(!isStarted && bIsEnabled) {
		xTaskCreateStatic( (TaskFunction_t) &telnet_task, "telnet", TELNET_STACK_SIZE, NULL, ESP_TASK_PRIO_MIN + 1, xStack, xTaskBuffer);
		isStarted=true;
	}
}
static void telnet_task(void *data) {
	int serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(23);

	int rc = bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
	if (rc < 0) {
		ESP_LOGE(tag, "bind: %d (%s)", errno, strerror(errno));
		close(serverSocket);
		return;
	}

	rc = listen(serverSocket, 5);
	if (rc < 0) {
		ESP_LOGE(tag, "listen: %d (%s)", errno, strerror(errno));
		close(serverSocket);
		return;
	}

	while(1) {
		socklen_t len = sizeof(serverAddr);
		rc = accept(serverSocket, (struct sockaddr *)&serverAddr, &len);
		if (rc < 0 ){
			ESP_LOGE(tag, "accept: %d (%s)", errno, strerror(errno));
			return;
		}
		else {
			partnerSocket = rc;
			ESP_LOGD(tag, "We have a new client connection!");
			handle_telnet_conn();
			ESP_LOGD(tag, "Telnet connection terminated");
		}
	}
	close(serverSocket);
	vTaskDelete(NULL);
}

/**
 * Convert a telnet event type to its string representation.
 */
static char *eventToString(telnet_event_type_t type) {
	switch(type) {
	case TELNET_EV_COMPRESS:
		return "TELNET_EV_COMPRESS";
	case TELNET_EV_DATA:
		return "TELNET_EV_DATA";
	case TELNET_EV_DO:
		return "TELNET_EV_DO";
	case TELNET_EV_DONT:
		return "TELNET_EV_DONT";
	case TELNET_EV_ENVIRON:
		return "TELNET_EV_ENVIRON";
	case TELNET_EV_ERROR:
		return "TELNET_EV_ERROR";
	case TELNET_EV_IAC:
		return "TELNET_EV_IAC";
	case TELNET_EV_MSSP:
		return "TELNET_EV_MSSP";
	case TELNET_EV_SEND:
		return "TELNET_EV_SEND";
	case TELNET_EV_SUBNEGOTIATION:
		return "TELNET_EV_SUBNEGOTIATION";
	case TELNET_EV_TTYPE:
		return "TELNET_EV_TTYPE";
	case TELNET_EV_WARNING:
		return "TELNET_EV_WARNING";
	case TELNET_EV_WILL:
		return "TELNET_EV_WILL";
	case TELNET_EV_WONT:
		return "TELNET_EV_WONT";
	case TELNET_EV_ZMP:
		return "TELNET_EV_ZMP";
	}
	return "Unknown type";
} // eventToString

/**
 * Telnet handler.
 */
void process_received_data(const char * buffer, size_t size){
	//ESP_LOGD(tag, "received data, len=%d", event->data.size);

	char * command = malloc(size+1);
	const char * c=buffer;

	// scrub from any escape command
	if(*c == '\e'){
		while(*(c++) !='n'){
			--size;
		};
		--size;
	}
	memcpy(command,c,size);
	command[size]='\0';
	if(command[0]!='\r' && command[0]!='\n'){
		// echo the command buffer out to uart and run
		if(bMirrorToUART){
			write(uart_fd, command, size);
		}
		run_command((char *)command);
	}
	free(command);

}
static void handle_telnet_events(
		telnet_t *thisTelnet,
		telnet_event_t *event,
		void *userData) {
	int rc;
	struct telnetUserData *telnetUserData = (struct telnetUserData *)userData;
	switch(event->type) {
	case TELNET_EV_SEND:
		rc = send(telnetUserData->sockfd, event->data.buffer, event->data.size, 0);
		if (rc < 0) {
			//printf("ERROR: (telnet) send: %d (%s)", errno, strerror(errno));
		}
		break;

	case TELNET_EV_DATA:
		 process_received_data(event->data.buffer, event->data.size);
		break;
	case TELNET_EV_TTYPE:
		printf("telnet event: %s\n", eventToString(event->type));
		telnet_ttype_send(telnetUserData->tnHandle);
		break;



	default:
		printf("telnet event: %s\n", eventToString(event->type));
		break;
	} // End of switch event type
} // myhandle_telnet_events


static void process_logs(UBaseType_t count){
    //Receive an item from no-split ring buffer
	size_t item_size;
    UBaseType_t uxItemsWaiting;
    UBaseType_t uxBytesToSend=count;

	vRingbufferGetInfo(buf_handle, NULL, NULL, NULL, NULL, &uxItemsWaiting);
	if(count == 0){
		// this sends the entire buffer to the remote client
		uxBytesToSend = uxItemsWaiting;
	}
	if( partnerSocket ==0 && (uxItemsWaiting*100 / log_buf_size) <75){
		// We still have some room in the ringbuffer and there's no telnet
		// connection yet, so bail out for now.
		//printf("%s() Log buffer used %u of %u bytes used\n", __FUNCTION__, uxItemsWaiting, log_buf_size);
		return;
	}

	while(uxBytesToSend>0){
		char *item = (char *)xRingbufferReceiveUpTo(buf_handle, &item_size, pdMS_TO_TICKS(50), uxBytesToSend);

		//Check received data
		if (item != NULL) {
			uxBytesToSend-=item_size;
			if(partnerSocket!=0){
				telnet_send_text(tnHandle, item, item_size);
			}
			//Return Item
			vRingbufferReturnItem(buf_handle, (void *)item);
		}
		else{
			break;
		}
	}
}

static void handle_telnet_conn() {

  static const telnet_telopt_t my_telopts[] = {
    { TELNET_TELOPT_ECHO,      TELNET_WONT, TELNET_DO },
    { TELNET_TELOPT_TTYPE,     TELNET_WILL, TELNET_DONT },
    { TELNET_TELOPT_COMPRESS2, TELNET_WONT, TELNET_DO   },
    { TELNET_TELOPT_ZMP,       TELNET_WONT, TELNET_DO   },
    { TELNET_TELOPT_MSSP,      TELNET_WONT, TELNET_DO   },
    { TELNET_TELOPT_BINARY,    TELNET_WILL, TELNET_DO   },
    { TELNET_TELOPT_NAWS,      TELNET_WILL, TELNET_DONT },
	{TELNET_TELOPT_LINEMODE,   TELNET_WONT, TELNET_DO },
    { -1, 0, 0 }
  };
  struct telnetUserData *pTelnetUserData = (struct telnetUserData *)malloc(sizeof(struct telnetUserData));
  tnHandle = telnet_init(my_telopts, handle_telnet_events, 0, pTelnetUserData);

  pTelnetUserData->rxbuf = (char *) heap_caps_malloc(TELNET_RX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  pTelnetUserData->tnHandle = tnHandle;
  pTelnetUserData->sockfd = partnerSocket;

  // flush all the log buffer on connect
  process_logs(0);

  while(1) {
  	//ESP_LOGD(tag, "waiting for data");
  	ssize_t len = recv(partnerSocket, pTelnetUserData->rxbuf, TELNET_RX_BUF, MSG_DONTWAIT);
  	if (len >0 ) {
		telnet_recv(tnHandle, pTelnetUserData->rxbuf, len);
  	}
  	else if (errno != EAGAIN && errno !=EWOULDBLOCK ){
  	  telnet_free(tnHandle);
  	  tnHandle = NULL;
  	  free(pTelnetUserData->rxbuf);
  	  pTelnetUserData->rxbuf=NULL;
  	  free(pTelnetUserData);
  	  partnerSocket = 0;
  	  return;
  	}
  	process_logs(send_chunk);

	taskYIELD();
  }

} // handle_telnet_conn

// ******************* stdout/stderr Redirection to ringbuffer
static ssize_t stdout_write(int fd, const void * data, size_t size) {
	if (xSemaphoreTake(xSemaphore, (TickType_t) 10) == pdTRUE) {
		// #1 Write to ringbuffer
		if (buf_handle == NULL) {
			printf("%s() ABORT. file handle _log_remote_fp is NULL\n",
					__FUNCTION__);
		} else {
			//Send an item
			UBaseType_t res = xRingbufferSend(buf_handle, data, size,
					pdMS_TO_TICKS(100));
			if (res != pdTRUE) {
				// flush some entries
				process_logs(size);
				res = xRingbufferSend(buf_handle, data, size,
						pdMS_TO_TICKS(100));
				if (res != pdTRUE) {

					printf("%s() ABORT. Unable to store log entry in buffer\n",
							__FUNCTION__);
				}
			}
		}
		xSemaphoreGive(xSemaphore);
	} else {
		// We could not obtain the semaphore and can therefore not access
		// the shared resource safely.
	}
	return bMirrorToUART?write(uart_fd, data, size):true;
}

static ssize_t stdout_read(int fd, void* data, size_t size) {
	return read(fd, data, size);
}

static int stdout_open(const char * path, int flags, int mode) {
	return 0;
}

static int stdout_close(int fd) {
	return 0;
}

static int stdout_fstat(int fd, struct stat * st) {
	st->st_mode = S_IFCHR;
	return 0;
}
