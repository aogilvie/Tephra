#include "EJBindingHttpRequest.h"
#include <queue>
#include <pthread.h>
#include <errno.h>

#include "curl/curl.h"

static pthread_t        s_networkThread;
static pthread_mutex_t  s_requestQueueMutex;
static pthread_mutex_t  s_responseQueueMutex;

static pthread_mutex_t		s_SleepMutex;
static pthread_cond_t		s_SleepCondition;

static unsigned long    s_asyncRequestCount = 0;

#ifdef _WINDOWS
typedef int int32_t;
#endif

static bool need_quit = false;

static NSArray* s_requestQueue = NULL;
static NSArray* s_responseQueue = NULL;

static EJHttpClient *s_pHttpClient = NULL; // pointer to singleton

static char s_errorBuffer[CURL_ERROR_SIZE];

typedef size_t (*write_callback)(void *ptr, size_t size, size_t nmemb, void *stream);

// Callback function used by libcurl for collect response data
size_t writeData(void *ptr, size_t size, size_t nmemb, void *stream)
{
    std::vector<char> *recvBuffer = (std::vector<char>*)stream;
    size_t sizes = size * nmemb;
    
    // add data to the end of recvBuffer
    // write data maybe called more than once in a single request
    recvBuffer->insert(recvBuffer->end(), (char*)ptr, (char*)ptr+sizes);
    
    return sizes;
}

// Prototypes
bool configureCURL(CURL *handle);
int processGetTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *errorCode);
int processPostTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *errorCode);
int processPutTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *errorCode);
int processDeleteTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *errorCode);
// int processDownloadTask(HttpRequest *task, write_callback callback, void *stream, int32_t *errorCode);


// Worker thread
static void* networkThread(void *data)
{    
    EJHttpRequest *request = NULL;
    
    while (true) 
    {
        if (need_quit)
        {
            break;
        }
        
        // step 1: send http request if the requestQueue isn't empty
        request = NULL;
        
        pthread_mutex_lock(&s_requestQueueMutex); //Get request task from queue
        if (0 != s_requestQueue->count())
        {
            request = dynamic_cast<EJHttpRequest*>(s_requestQueue->objectAtIndex(0));
            s_requestQueue->removeObjectAtIndex(0);  
            // request's refcount = 1 here
        }
        pthread_mutex_unlock(&s_requestQueueMutex);
        
        if (NULL == request)
        {
        	// Wait for http request tasks from main thread
        	pthread_cond_wait(&s_SleepCondition, &s_SleepMutex);
            continue;
        }
        
        // step 2: libcurl sync access
        
        // Create a HttpResponse object, the default setting is http access failed
        EJHttpResponse *response = new EJHttpResponse(request);
        
        // request's refcount = 2 here, it's retained by HttpRespose constructor
        request->release();
        // ok, refcount = 1 now, only HttpResponse hold it.
        
        int32_t responseCode = -1;
        int retValue = 0;

        // Process the request -> get response packet
        switch (request->getRequestType())
        {
            case EJHttpRequest::kHttpGet: // HTTP GET
                retValue = processGetTask(request, 
                                          writeData, 
                                          response->getResponseData(), 
                                          &responseCode);
                break;
            
            case EJHttpRequest::kHttpPost: // HTTP POST
                retValue = processPostTask(request, 
                                           writeData, 
                                           response->getResponseData(), 
                                           &responseCode);
                break;

            case EJHttpRequest::kHttpPut:
                retValue = processPutTask(request,
                                          writeData,
                                          response->getResponseData(),
                                          &responseCode);
                break;

            case EJHttpRequest::kHttpDelete:
                retValue = processDeleteTask(request,
                                             writeData,
                                             response->getResponseData(),
                                             &responseCode);
                break;
            
            default:
                NSAssert(true, "EJHttpClient: unkown request type, only GET and POSt are supported");
                break;
        }
                
        // write data to HttpResponse
        response->setResponseCode(responseCode);
        
        if (retValue != 0) 
        {
            response->setSucceed(false);
            response->setErrorBuffer(s_errorBuffer);
        }
        else
        {
            response->setSucceed(true);
        }

        
        // add response packet into queue
        pthread_mutex_lock(&s_responseQueueMutex);
        s_responseQueue->addObject(response);
        pthread_mutex_unlock(&s_responseQueueMutex);
        
        // resume dispatcher selector
        //EJDirector::sharedDirector()->getScheduler()->resumeTarget(EJHttpClient::getInstance());
    }
    
    // cleanup: if worker thread received quit signal, clean up un-completed request queue
    pthread_mutex_lock(&s_requestQueueMutex);
    s_requestQueue->removeAllObjects();
    pthread_mutex_unlock(&s_requestQueueMutex);
    s_asyncRequestCount -= s_requestQueue->count();
    
    if (s_requestQueue != NULL) {
        
        pthread_mutex_destroy(&s_requestQueueMutex);
        pthread_mutex_destroy(&s_responseQueueMutex);
        
        pthread_mutex_destroy(&s_SleepMutex);
        pthread_cond_destroy(&s_SleepCondition);

        s_requestQueue->release();
        s_requestQueue = NULL;
        s_responseQueue->release();
        s_responseQueue = NULL;
    }

    pthread_exit(NULL);
    
    return 0;
}

//Configure curl's timeout property
bool configureCURL(CURL *handle)
{
    if (!handle) {
        return false;
    }
    
    int32_t code;
    code = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, s_errorBuffer);
    if (code != CURLE_OK) {
        return false;
    }
    code = curl_easy_setopt(handle, CURLOPT_TIMEOUT, EJHttpClient::getInstance()->getTimeoutForRead());
    if (code != CURLE_OK) {
        return false;
    }
    code = curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, EJHttpClient::getInstance()->getTimeoutForConnect());
    if (code != CURLE_OK) {
        return false;
    }
    
    return true;
}

class CURLRaii
{
    /// Instance of CURL
    CURL *m_curl;
    /// Keeps custom header data
    curl_slist *m_headers;
public:
    CURLRaii()
        : m_curl(curl_easy_init())
        , m_headers(NULL)
    {
    }

    ~CURLRaii()
    {
        if (m_curl)
            curl_easy_cleanup(m_curl);
        /* free the linked list for header data */
        if (m_headers)
            curl_slist_free_all(m_headers);
    }

    template <class T>
    bool setOption(CURLoption option, T data)
    {
        return CURLE_OK == curl_easy_setopt(m_curl, option, data);
    }

    /**
     * @brief Inits CURL instance for common usage
     * @param request Null not allowed
     * @param callback Response write callback
     * @param stream Response write stream
     */
    bool init(EJHttpRequest *request, write_callback callback, void *stream)
    {
        if (!m_curl)
            return false;
        if (!configureCURL(m_curl))
            return false;

        /* get custom header data (if set) */
       	std::vector<std::string> headers=request->getHeaders();
        if(!headers.empty())
        {
            /* append custom headers one by one */
            for (std::vector<std::string>::iterator it = headers.begin(); it != headers.end(); ++it)
                m_headers = curl_slist_append(m_headers,it->c_str());
            /* set custom headers for curl */
            if (!setOption(CURLOPT_HTTPHEADER, m_headers))
                return false;
        }

        return setOption(CURLOPT_URL, request->getUrl())
                && setOption(CURLOPT_WRITEFUNCTION, callback)
                && setOption(CURLOPT_WRITEDATA, stream);
    }

    /// @param responseCode Null not allowed
    bool perform(int *responseCode)
    {
        if (CURLE_OK != curl_easy_perform(m_curl))
            return false;
        CURLcode code = curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, responseCode);
        if (code != CURLE_OK || *responseCode != 200)
            return false;
        return true;
    }
};

//Process Get Request
int processGetTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *responseCode)
{
    CURLRaii curl;
    bool ok = curl.init(request, callback, stream)
            && curl.setOption(CURLOPT_FOLLOWLOCATION, true)
            && curl.perform(responseCode);
    return ok ? 0 : 1;
}

//Process POST Request
int processPostTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *responseCode)
{
    CURLRaii curl;
    bool ok = curl.init(request, callback, stream)
            && curl.setOption(CURLOPT_POST, 1)
            && curl.setOption(CURLOPT_POSTFIELDS, request->getRequestData())
            && curl.setOption(CURLOPT_POSTFIELDSIZE, request->getRequestDataSize())
            && curl.perform(responseCode);
    return ok ? 0 : 1;
}

//Process PUT Request
int processPutTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *responseCode)
{
    CURLRaii curl;
    bool ok = curl.init(request, callback, stream)
            && curl.setOption(CURLOPT_CUSTOMREQUEST, "PUT")
            && curl.setOption(CURLOPT_POSTFIELDS, request->getRequestData())
            && curl.setOption(CURLOPT_POSTFIELDSIZE, request->getRequestDataSize())
            && curl.perform(responseCode);
    return ok ? 0 : 1;
}

//Process DELETE Request
int processDeleteTask(EJHttpRequest *request, write_callback callback, void *stream, int32_t *responseCode)
{
    CURLRaii curl;
    bool ok = curl.init(request, callback, stream)
            && curl.setOption(CURLOPT_CUSTOMREQUEST, "DELETE")
            && curl.setOption(CURLOPT_FOLLOWLOCATION, true)
            && curl.perform(responseCode);
    return ok ? 0 : 1;
}
// HttpClient implementation
EJHttpClient* EJHttpClient::getInstance()
{
    if (s_pHttpClient == NULL) {
        s_pHttpClient = new EJHttpClient();
    }
    
    return s_pHttpClient;
}

void EJHttpClient::destroyInstance()
{
    NSAssert(s_pHttpClient, "");
    s_pHttpClient->release();
}

EJHttpClient::EJHttpClient()
: _timeoutForConnect(30)
, _timeoutForRead(60)
{
}

EJHttpClient::~EJHttpClient()
{
    need_quit = true;
    
    if (s_requestQueue != NULL) {
    	pthread_cond_signal(&s_SleepCondition);
    }
    
    s_pHttpClient = NULL;
}

//Lazy create semaphore & mutex & thread
bool EJHttpClient::lazyInitThreadSemphore()
{
    if (s_requestQueue != NULL) {
        return true;
    } else {
        
        s_requestQueue = new NSArray();
        s_requestQueue->init();
        
        s_responseQueue = new NSArray();
        s_responseQueue->init();
        
        pthread_mutex_init(&s_requestQueueMutex, NULL);
        pthread_mutex_init(&s_responseQueueMutex, NULL);
        
        pthread_mutex_init(&s_SleepMutex, NULL);
        pthread_cond_init(&s_SleepCondition, NULL);

        pthread_create(&s_networkThread, NULL, networkThread, NULL);
        pthread_detach(s_networkThread);
        
        need_quit = false;
    }
    
    return true;
}

//Add a get task to queue
void EJHttpClient::send(EJHttpRequest* request)
{    
    if (false == lazyInitThreadSemphore()) 
    {
        return;
    }
    
    if (!request)
    {
        return;
    }
        
    ++s_asyncRequestCount;
    
    request->retain();
        
    pthread_mutex_lock(&s_requestQueueMutex);
    s_requestQueue->addObject(request);
    pthread_mutex_unlock(&s_requestQueueMutex);
    
    // Notify thread start to work
    pthread_cond_signal(&s_SleepCondition);
}

// Poll and notify main thread if responses exists in queue
void EJHttpClient::dispatchResponseCallbacks(float delta)
{
    
    if (0 == s_asyncRequestCount) 
    {
        return;
    }

    //NSLOG("EJHttpClient::dispatchResponseCallbacks is running");
    
    EJHttpResponse* response = NULL;
    
    pthread_mutex_lock(&s_responseQueueMutex);
    if (s_responseQueue->count())
    {
        response = dynamic_cast<EJHttpResponse*>(s_responseQueue->objectAtIndex(0));
        s_responseQueue->removeObjectAtIndex(0);
    }
    pthread_mutex_unlock(&s_responseQueueMutex);
    
    if (response)
    {
        --s_asyncRequestCount;
        
        EJHttpRequest *request = response->getHttpRequest();
        NSObject *pTarget = request->getTarget();
        SEL_HttpResponse pSelector = request->getSelector();

        if (pTarget && pSelector) 
        {
            (pTarget->*pSelector)(this, response);
        }
        
        response->release();
    }
    
}

EJBindingHttpRequest::EJBindingHttpRequest() :method(NULL), url(NULL), user(NULL), password(NULL), connection(NULL),response(NULL),responseBody(NULL)
{
	requestHeaders = new NSDictionary();
}

EJBindingHttpRequest::~EJBindingHttpRequest() {
	requestHeaders->release();
	clearRequest();
	clearConnection();
}

void EJBindingHttpRequest::init(JSContextRef ctx ,JSObjectRef obj, size_t argc, const JSValueRef argv[]) {
}

void EJBindingHttpRequest::clearConnection()
{
	if(connection)connection->release(); connection = NULL;
	if(responseBody)delete [] responseBody; responseBody = NULL;
	if(response)response->release(); response = NULL;
}

void EJBindingHttpRequest::clearRequest()
{
	if(method)method->release(); method = NULL;
	if(url)url->release(); url = NULL;
	if(user)user->release(); user = NULL;
	if(password)password->release(); password = NULL;
}

int EJBindingHttpRequest::getStatusCode()
{
	if( !response ) {
		return 0;
	}
	else if( response->getResponseCode() != 200 ) {
		return response->getResponseCode();
	}
	else {
		return 200; // assume everything went well for non-HTTP resources
	}
}

NSString * EJBindingHttpRequest::getResponseText()
{
	if( !response || !responseBody ) { return NULL; }

	// NSStringEncoding encoding = NSASCIIStringEncoding;
	// if ( response.textEncodingName ) {
	// 	CFStringEncoding cfEncoding = CFStringConvertIANACharSetNameToEncoding((CFStringRef) [response textEncodingName]);
	// 	if( cfEncoding != kCFStringEncodingInvalidId ) {
	// 		encoding = CFStringConvertEncodingToNSStringEncoding(cfEncoding);
	// 	}
	// }

	return NSStringMake(responseBody);
}

void EJBindingHttpRequest::onHttpRequestCompleted(NSObject *sender, void *data)
{
	response = (EJHttpResponse*)data;

	if (!response)
	{
		return;
	}

    state = kEJHttpRequestStateDone;

	// You can get original request type from: response->request->reqType
	if (0 != strlen(response->getHttpRequest()->getTag())) 
	{
		NSLOG("%s completed", response->getHttpRequest()->getTag());
	}

	int statusCode = response->getResponseCode();
	char statusString[64] = {};
	sprintf(statusString, "HTTP Status Code: %d, tag = %s", statusCode, response->getHttpRequest()->getTag());

	NSLOG("response code: %d", statusCode);

	if (!response->isSucceed()) 
	{
		NSLOG("response failed");
		NSLOG("error buffer: %s", response->getErrorBuffer());
		response->retain();
		responseBody = NULL;
        EJBindingEventedBase::triggerEvent(NSStringMake("loadend"), 0, NULL);
        EJBindingEventedBase::triggerEvent(NSStringMake("readystatechange"), 0, NULL);
		return;
	}

	// dump data
	std::vector<char> *buffer = response->getResponseData();
	printf("Http Test, dump data: ");
	for (unsigned int i = 0; i < buffer->size(); i++)
	{
		printf("%c", (*buffer)[i]);
	}
	printf("\n");

	response->retain();
	std::string buf = std::string(buffer->begin(), buffer->end());
	responseBody = new char[buffer->size()];
	sprintf(responseBody, "%s", buf.c_str());

    EJBindingEventedBase::triggerEvent(NSStringMake("loadend"), 0, NULL);
    EJBindingEventedBase::triggerEvent(NSStringMake("readystatechange"), 0, NULL);
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, open, ctx, argc, argv) {	
	if( argc < 2 ) { return NULL; }

	// Cleanup previous request, if any
	clearConnection();
	clearRequest();

	method = JSValueToNSString( ctx, argv[0] ); method->retain();
	url = JSValueToNSString( ctx, argv[1] ); url->retain();
	async = argc > 2 ? JSValueToBoolean( ctx, argv[2] ) : true;

	if( argc > 4 ) {
		user = JSValueToNSString( ctx, argv[3] ); user->retain();
		password = JSValueToNSString( ctx, argv[4] ); password->retain();
	}

	state = kEJHttpRequestStateOpened;
	return NULL;
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, setRequestHeader, ctx, argc, argv) {
	if( argc < 2 ) { return NULL; }

	NSString * header = JSValueToNSString( ctx, argv[0] );
	NSString * value = JSValueToNSString( ctx, argv[1] );

	requestHeaders->setObject(value, header->getCString());
	return NULL;
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, abort, ctx, argc, argv) {
	if( connection ) {
		clearConnection();
		EJBindingEventedBase::triggerEvent(NSStringMake("abort"), 0, NULL);
	}
	return NULL;
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, getAllResponseHeaders, ctx, argc, argv) {
	if( !response ) {
		return NULL;
	}

	NSString * headers = NSStringMake("");

	return NSStringToJSValue(ctx, headers);
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, getResponseHeader, ctx, argc, argv) {
	if( argc < 1 || !response ) {
		return NULL;
	}

	NSString * header = JSValueToNSString( ctx, argv[0] );
	NSString * value = header;

	return value ? NSStringToJSValue(ctx, value) : NULL;
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, overrideMimeType, ctx, argc, argv) {
	// TODO?
	return NULL;
}

EJ_BIND_FUNCTION(EJBindingHttpRequest, send, ctx, argc, argv) {
	if( !method || !url ) { return NULL; }

	clearConnection();

	// NSURL * requestUrl = [NSURL URLWithString:url];
	// if( !requestUrl.host ) {
	// 	// No host? Assume we have a local file
	// 	requestUrl = [NSURL fileURLWithPath:[[EJApp instance] pathForResource:url]];
	// }
	// NSMutableURLRequest * request = [[NSMutableURLRequest alloc] initWithURL:requestUrl];
	// [request setHTTPMethod:method];

	// for( NSString * header in requestHeaders ) {
	// 	[request setValue:[requestHeaders objectForKey:header] forHTTPHeaderField:header];
	// }

	// if( argc > 0 ) {
	// 	NSString * requestBody = JSValueToNSString( ctx, argv[0] );
	// 	NSData * requestData = [NSData dataWithBytes:[requestBody UTF8String] length:[requestBody length]];
	// 	[request setHTTPBody:requestData];
	// }

	// if( timeout ) {
	// 	NSTimeInterval timeoutSeconds = (float)timeout/1000.0f;
	// 	[request setTimeoutInterval:timeoutSeconds];
	// }	


	EJHttpRequest* request = new EJHttpRequest();
	request->setUrl(url->getCString());
	if (method->isEqual(NSStringMake("GET")))
	{
		request->setRequestType(EJHttpRequest::kHttpGet);
	}
	else if(method->isEqual(NSStringMake("POST")))
	{
		request->setRequestType(EJHttpRequest::kHttpPost);
	}
	else if(method->isEqual(NSStringMake("PUT")))
	{
		request->setRequestType(EJHttpRequest::kHttpPut);
	}
	else if(method->isEqual(NSStringMake("DELETE")))
	{
		request->setRequestType(EJHttpRequest::kHttpDelete);
	}
	else
	{
		request->setRequestType(EJHttpRequest::kHttpUnkown);
	}
	request->setResponseCallback(this, callfuncND_selector(EJBindingHttpRequest::onHttpRequestCompleted));

	connection = EJHttpClient::getInstance();
	connection->setTimeoutForConnect(timeout/1000);

	NSLOG("XHR: %s %s", method->getCString(), url->getCString());
	EJBindingEventedBase::triggerEvent(NSStringMake("loadstart"), 0, NULL);

	// if( async ) {
	// 	state = kEJHttpRequestStateLoading;
	// 	connection = [[NSURLConnection alloc] initWithRequest:request delegate:self];
	// }
	// else {	
	// 	NSData * data = [NSURLConnection sendSynchronousRequest:request returningResponse:&response error:nil];
	// 	responseBody = [[NSMutableData alloc] initWithData:data];
	// 	[response retain];

	// 	state = kEJHttpRequestStateDone;
	// 	if( [response isKindOfClass:[NSHTTPURLResponse class]] ) {
	// 		NSHTTPURLResponse * urlResponse = (NSHTTPURLResponse *)response;
	// 		if( urlResponse.statusCode == 200 ) {
	// 			[self triggerEvent:@"load" argc:0 argv:NULL];
	// 		}
	// 	}
	// 	else {
	// 		[self triggerEvent:@"load" argc:0 argv:NULL];
	// 	}
	// 	[self triggerEvent:@"loadend" argc:0 argv:NULL];
	// 	[self triggerEvent:@"readystatechange" argc:0 argv:NULL];
	// }
	// [request release];

    state = kEJHttpRequestStateLoading;
	connection->send(request);
	request->release();
	EJBindingEventedBase::triggerEvent(NSStringMake("load"), 0, NULL);

	return NULL;
}

EJ_BIND_GET(EJBindingHttpRequest, readyState, ctx) {
	return JSValueMakeNumber( ctx, state );
}

EJ_BIND_GET(EJBindingHttpRequest, response, ctx) {
	NSString * responseText = getResponseText();
	if( !responseText ) { return NULL; }

	// if( type == kEJHttpRequestTypeJSON ) {
	// 	JSStringRef jsText = JSStringCreateWithCFString((CFStringRef)responseText);
	// 	JSObjectRef jsonObject = (JSObjectRef)JSValueMakeFromJSONString(ctx, jsText);
	// 	JSStringRelease(jsText);
	// 	return jsonObject;
	// }
	// else {
		return NSStringToJSValue( ctx, responseText );
	//}
}

EJ_BIND_GET(EJBindingHttpRequest, responseText, ctx) {
	NSString * responseText = getResponseText();	
	return responseText ? NSStringToJSValue( ctx, responseText ) : NULL;
}

EJ_BIND_GET(EJBindingHttpRequest, status, ctx) {
	return JSValueMakeNumber( ctx, getStatusCode() );
}

EJ_BIND_GET(EJBindingHttpRequest, statusText, ctx) {
	// FIXME: should be "200 OK" instead of just "200"

 	NSString * code = new NSString();
 	code->autorelease();
 	code->initWithFormat("%d",getStatusCode());
	return NSStringToJSValue(ctx, code);
}

EJ_BIND_GET(EJBindingHttpRequest, timeout, ctx) {
	return JSValueMakeNumber( ctx, timeout );
}

EJ_BIND_SET(EJBindingHttpRequest, timeout, ctx, value) {
	timeout = JSValueToNumberFast( ctx, value );
}

//EJ_BIND_ENUM(EJBindingHttpRequest, responseType, EJHttpRequestTypeNames, type);

EJ_BIND_CONST(UNSENT, kEJHttpRequestStateUnsent);
EJ_BIND_CONST(OPENED, kEJHttpRequestStateOpened);
EJ_BIND_CONST(HEADERS_RECEIVED, kEJHttpRequestStateHeadersReceived);
EJ_BIND_CONST(LOADING, kEJHttpRequestStateLoading);
EJ_BIND_CONST(DONE, kEJHttpRequestStateDone);

EJ_BIND_EVENT(EJBindingHttpRequest, readystatechange);
EJ_BIND_EVENT(EJBindingHttpRequest, loadend);
EJ_BIND_EVENT(EJBindingHttpRequest, load);
EJ_BIND_EVENT(EJBindingHttpRequest, error);
EJ_BIND_EVENT(EJBindingHttpRequest, abort);
EJ_BIND_EVENT(EJBindingHttpRequest, progress);
EJ_BIND_EVENT(EJBindingHttpRequest, loadstart);
EJ_BIND_EVENT(EJBindingHttpRequest, timeout);

REFECTION_CLASS_IMPLEMENT(EJBindingHttpRequest);
