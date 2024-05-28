#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RadioLibInterface.h"
#include "airtime.h"
#include "main.h"
#include "mesh/http/ContentHelper.h"
#include "mesh/http/WebServer.h"
#if !MESHTASTIC_EXCLUDE_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "esp_err.h"
#include "esp_flash_partitions.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "mqtt/JSON.h"
#include "power.h"
#include "sleep.h"
#include <FS.h>
#include <FSCommon.h>
#include <HTTPBodyParser.hpp>
#include <HTTPMultipartBodyParser.hpp>
#include <HTTPURLEncodedBodyParser.hpp>
#include <SPIFFS.h>
#include <Update.h>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

#undef str

// Includes for the https server
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>
#include <HTTPServer.hpp>
#include <SSLCert.hpp>

// The HTTPS Server comes in a separate namespace. For easier use, include it here.
using namespace httpsserver;

#include "mesh/http/ContentHandler.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
HTTPClient httpClient;

#define DEST_FS_USES_LITTLEFS

char contentTypes[][2][32] = {{".txt", "text/plain"},     {".html", "text/html"},
                              {".js", "text/javascript"}, {".png", "image/png"},
                              {".jpg", "image/jpg"},      {".gz", "application/gzip"},
                              {".gif", "image/gif"},      {".json", "application/json"},
                              {".css", "text/css"},       {".ico", "image/vnd.microsoft.icon"},
                              {".svg", "image/svg+xml"},  {"", ""}};

// Our API to handle messages to and from the radio.
HttpAPI webAPI;

HTTPServer *insecureServer;
HTTPSServer *secureServer;

SSLCert *sslCert;

namespace ContentHandler
{

void setup()
{
    Serial.begin(115200);

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("An error has occurred while mounting SPIFFS");
        return;
    }

    // Read certificate and key files
    File certFile = SPIFFS.open("/certs/cert.pem", "r");
    File keyFile = SPIFFS.open("/certs/key.pem", "r");

    if (!certFile || !keyFile) {
        Serial.println("Failed to open certificate or key file");
        return;
    }

    size_t certSize = certFile.size();
    size_t keySize = keyFile.size();

    unsigned char *certData = (unsigned char *)malloc(certSize);
    unsigned char *keyData = (unsigned char *)malloc(keySize);

    certFile.read(certData, certSize);
    keyFile.read(keyData, keySize);

    certFile.close();
    keyFile.close();

    // Initialize SSL certificate
    sslCert = new SSLCert(certData, certSize, keyData, keySize);

    // Create the HTTP server instances
    insecureServer = new HTTPServer();
    secureServer = new HTTPSServer(sslCert, 443);

    // Register handlers
    registerHandlers(insecureServer, secureServer);

    // Start the servers
    insecureServer->start();
    secureServer->start();

    if (insecureServer->isRunning()) {
        Serial.println("Insecure HTTP server started");
    } else {
        Serial.println("Failed to start insecure HTTP server");
    }

    if (secureServer->isRunning()) {
        Serial.println("Secure HTTPS server started");
    } else {
        Serial.println("Failed to start secure HTTPS server");
    }
}

void loop()
{
    // Handle client connections for the servers
    insecureServer->loop();
    secureServer->loop();
    delay(100); // Minimal delay to avoid excessive CPU usage
}

} // namespace ContentHandler

void handleAPIv1FromRadio(HTTPRequest *req, HTTPResponse *res)
{
    LOG_DEBUG("webAPI handleAPIv1FromRadio\n");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    // std::string paramAll = "all";
    std::string valueAll;

    // Status code is 200 OK by default.
    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/mesh.proto");

    uint8_t txBuf[MAX_STREAM_BUF_SIZE];
    uint32_t len = 1;

    if (params->getQueryParameter("all", valueAll)) {

        // If all is true, return all the buffers we have available
        //   to us at this point in time.
        if (valueAll == "true") {
            while (len) {
                len = webAPI.getFromRadio(txBuf);
                res->write(txBuf, len);
            }

            // Otherwise, just return one protobuf
        } else {
            len = webAPI.getFromRadio(txBuf);
            res->write(txBuf, len);
        }

        // the param "all" was not specified. Return just one protobuf
    } else {
        len = webAPI.getFromRadio(txBuf);
        res->write(txBuf, len);
    }

    LOG_DEBUG("webAPI handleAPIv1FromRadio, len %d\n", len);
}

void handleAPIv1ToRadio(HTTPRequest *req, HTTPResponse *res)
{
    LOG_DEBUG("webAPI handleAPIv1ToRadio\n");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Headers", "Content-Type");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "PUT, OPTIONS");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        // res->print(""); @todo remove
        return;
    }

    byte buffer[MAX_TO_FROM_RADIO_SIZE];
    size_t s = req->readBytes(buffer, MAX_TO_FROM_RADIO_SIZE);

    LOG_DEBUG("Received %d bytes from PUT request\n", s);
    webAPI.handleToRadio(buffer, s);

    res->write(buffer, s);
    LOG_DEBUG("webAPI handleAPIv1ToRadio\n");
}

void htmlDeleteDir(const char *dirname)
{
    File root = FSCom.open(dirname);
    if (!root) {
        return;
    }
    if (!root.isDirectory()) {
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            htmlDeleteDir(file.name());
            file.flush();
            file.close();
        } else {
            String fileName = String(file.name());
            file.flush();
            file.close();
            LOG_DEBUG("    %s\n", fileName.c_str());
            FSCom.remove(fileName);
        }
        file = root.openNextFile();
    }
    root.flush();
    root.close();
}

JSONArray htmlListDir(const char *dirname, uint8_t levels)
{
    File root = FSCom.open(dirname, FILE_O_READ);
    JSONArray fileList;
    if (!root) {
        return fileList;
    }
    if (!root.isDirectory()) {
        return fileList;
    }

    // iterate over the file list
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                fileList.push_back(new JSONValue(htmlListDir(file.path(), levels - 1)));
#else
                fileList.push_back(new JSONValue(htmlListDir(file.name(), levels - 1)));
#endif
                file.close();
            }
        } else {
            JSONObject thisFileMap;
            thisFileMap["size"] = new JSONValue((int)file.size());
#ifdef ARCH_ESP32
            thisFileMap["name"] = new JSONValue(String(file.path()).substring(1).c_str());
#else
            thisFileMap["name"] = new JSONValue(String(file.name()).substring(1).c_str());
#endif
            if (String(file.name()).substring(1).endsWith(".gz")) {
#ifdef ARCH_ESP32
                String modifiedFile = String(file.path()).substring(1);
#else
                String modifiedFile = String(file.name()).substring(1);
#endif
                modifiedFile.remove((modifiedFile.length() - 3), 3);
                thisFileMap["nameModified"] = new JSONValue(modifiedFile.c_str());
            }
            fileList.push_back(new JSONValue(thisFileMap));
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return fileList;
}

void handleFsBrowseStatic(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    auto fileList = htmlListDir("/static", 10);

    // create json output structure
    JSONObject filesystemObj;
    filesystemObj["total"] = new JSONValue((int)FSCom.totalBytes());
    filesystemObj["used"] = new JSONValue((int)FSCom.usedBytes());
    filesystemObj["free"] = new JSONValue(int(FSCom.totalBytes() - FSCom.usedBytes()));

    JSONObject jsonObjInner;
    jsonObjInner["files"] = new JSONValue(fileList);
    jsonObjInner["filesystem"] = new JSONValue(filesystemObj);

    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(jsonObjInner);
    jsonObjOuter["status"] = new JSONValue("ok");

    JSONValue *value = new JSONValue(jsonObjOuter);

    res->print(value->Stringify().c_str());

    delete value;
}

void handleFsDeleteStatic(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string paramValDelete;

    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "DELETE");
    if (params->getQueryParameter("delete", paramValDelete)) {
        std::string pathDelete = "/" + paramValDelete;
        if (FSCom.remove(pathDelete.c_str())) {
            LOG_INFO("%s\n", pathDelete.c_str());
            JSONObject jsonObjOuter;
            jsonObjOuter["status"] = new JSONValue("ok");
            JSONValue *value = new JSONValue(jsonObjOuter);
            res->print(value->Stringify().c_str());
            delete value;
            return;
        } else {
            LOG_INFO("%s\n", pathDelete.c_str());
            JSONObject jsonObjOuter;
            jsonObjOuter["status"] = new JSONValue("Error");
            JSONValue *value = new JSONValue(jsonObjOuter);
            res->print(value->Stringify().c_str());
            delete value;
            return;
        }
    }
}

void handleStatic(HTTPRequest *req, HTTPResponse *res)
{
    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    std::string parameter1;
    // Print the first parameter value
    if (params->getPathParameter(0, parameter1)) {

        std::string filename = "/static/" + parameter1;
        std::string filenameGzip = "/static/" + parameter1 + ".gz";

        // Try to open the file
        File file;

        bool has_set_content_type = false;

        if (filename == "/static/") {
            filename = "/static/index.html";
            filenameGzip = "/static/index.html.gz";
        }

        if (FSCom.exists(filename.c_str())) {
            file = FSCom.open(filename.c_str());
            if (!file.available()) {
                LOG_WARN("File not available - %s\n", filename.c_str());
            }
        } else if (FSCom.exists(filenameGzip.c_str())) {
            file = FSCom.open(filenameGzip.c_str());
            res->setHeader("Content-Encoding", "gzip");
            if (!file.available()) {
                LOG_WARN("File not available - %s\n", filenameGzip.c_str());
            }
        } else {
            has_set_content_type = true;
            filenameGzip = "/static/index.html.gz";
            file = FSCom.open(filenameGzip.c_str());
            res->setHeader("Content-Type", "text/html");
            if (!file.available()) {
                LOG_WARN("File not available - %s\n", filenameGzip.c_str());
                res->println("Web server is running.<br><br>The content you are looking for can't be found. Please see: <a "
                             "href=https://meshtastic.org/docs/software/web-client/>FAQ</a>.<br><br><a "
                             "href=/admin>admin</a>");

                return;
            } else {
                res->setHeader("Content-Encoding", "gzip");
            }
        }

        res->setHeader("Content-Length", httpsserver::intToString(file.size()));

        // Content-Type is guessed using the definition of the contentTypes-table defined above
        int cTypeIdx = 0;
        do {
            if (filename.rfind(contentTypes[cTypeIdx][0]) != std::string::npos) {
                res->setHeader("Content-Type", contentTypes[cTypeIdx][1]);
                has_set_content_type = true;
                break;
            }
            cTypeIdx += 1;
        } while (strlen(contentTypes[cTypeIdx][0]) > 0);

        if (!has_set_content_type) {
            // Set a default content type
            res->setHeader("Content-Type", "application/octet-stream");
        }

        // Read the file and write it to the HTTP response body
        size_t length = 0;
        do {
            char buffer[256];
            length = file.read((uint8_t *)buffer, 256);
            std::string bufferString(buffer, length);
            res->write((uint8_t *)bufferString.c_str(), bufferString.size());
        } while (length > 0);

        file.close();

        return;
    } else {
        LOG_ERROR("This should not have happened...\n");
        res->println("ERROR: This should not have happened...");
    }
}

void handleFormUpload(HTTPRequest *req, HTTPResponse *res)
{
    LOG_DEBUG("Form Upload - Disabling keep-alive\n");
    res->setHeader("Connection", "close");

    LOG_DEBUG("Form Upload - Creating body parser reference\n");
    HTTPBodyParser *parser;
    std::string contentType = req->getHeader("Content-Type");

    size_t semicolonPos = contentType.find(";");
    if (semicolonPos != std::string::npos) {
        contentType.resize(semicolonPos);
    }

    if (contentType == "multipart/form-data") {
        LOG_DEBUG("Form Upload - multipart/form-data\n");
        parser = new HTTPMultipartBodyParser(req);
    } else {
        LOG_DEBUG("Unknown POST Content-Type: %s\n", contentType.c_str());
        return;
    }

    res->println("<html><head><meta http-equiv=\"refresh\" content=\"1;url=/static\" /><title>File "
                 "Upload</title></head><body><h1>File Upload</h1>");

    bool didwrite = false;

    while (parser->nextField()) {
        std::string name = parser->getFieldName();
        std::string filename = parser->getFieldFilename();
        std::string mimeType = parser->getFieldMimeType();
        LOG_DEBUG("handleFormUpload: field name='%s', filename='%s', mimetype='%s'\n", name.c_str(), filename.c_str(),
                  mimeType.c_str());

        if (name != "file") {
            LOG_DEBUG("Skipping unexpected field\n");
            res->println("<p>No file found.</p>");
            return;
        }

        if (filename == "") {
            LOG_DEBUG("Skipping unexpected field\n");
            res->println("<p>No file found.</p>");
            return;
        }

        std::string pathname = "/static/" + filename;

        File file = FSCom.open(pathname.c_str(), FILE_O_WRITE);
        size_t fileLength = 0;
        didwrite = true;

        while (!parser->endOfField()) {
            esp_task_wdt_reset();

            byte buf[512];
            size_t readLength = parser->read(buf, 512);

            if (FSCom.totalBytes() - FSCom.usedBytes() < 51200) {
                file.flush();
                file.close();
                res->println("<p>Write aborted! Reserving 50k on filesystem.</p>");

                delete parser;
                return;
            }

            file.write(buf, readLength);
            fileLength += readLength;
            LOG_DEBUG("File Length %i\n", fileLength);
        }

        file.flush();
        file.close();
        res->printf("<p>Saved %d bytes to %s</p>", (int)fileLength, pathname.c_str());
    }
    if (!didwrite) {
        res->println("<p>Did not write any file</p>");
    }
    res->println("</body></html>");
    delete parser;
}

void handleReport(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    // data->airtime->tx_log
    JSONArray txLogValues;
    uint32_t *logArray;
    logArray = airTime->airtimeReport(TX_LOG);
    for (int i = 0; i < airTime->getPeriodsToLog(); i++) {
        txLogValues.push_back(new JSONValue((int)logArray[i]));
    }

    // data->airtime->rx_log
    JSONArray rxLogValues;
    logArray = airTime->airtimeReport(RX_LOG);
    for (int i = 0; i < airTime->getPeriodsToLog(); i++) {
        rxLogValues.push_back(new JSONValue((int)logArray[i]));
    }

    // data->airtime->rx_all_log
    JSONArray rxAllLogValues;
    logArray = airTime->airtimeReport(RX_ALL_LOG);
    for (int i = 0; i < airTime->getPeriodsToLog(); i++) {
        rxAllLogValues.push_back(new JSONValue((int)logArray[i]));
    }

    // data->airtime
    JSONObject jsonObjAirtime;
    jsonObjAirtime["tx_log"] = new JSONValue(txLogValues);
    jsonObjAirtime["rx_log"] = new JSONValue(rxLogValues);
    jsonObjAirtime["rx_all_log"] = new JSONValue(rxAllLogValues);
    jsonObjAirtime["channel_utilization"] = new JSONValue(airTime->channelUtilizationPercent());
    jsonObjAirtime["utilization_tx"] = new JSONValue(airTime->utilizationTXPercent());
    jsonObjAirtime["seconds_since_boot"] = new JSONValue(int(airTime->getSecondsSinceBoot()));
    jsonObjAirtime["seconds_per_period"] = new JSONValue(int(airTime->getSecondsPerPeriod()));
    jsonObjAirtime["periods_to_log"] = new JSONValue(airTime->getPeriodsToLog());

    // data->wifi
    JSONObject jsonObjWifi;
    jsonObjWifi["rssi"] = new JSONValue(WiFi.RSSI());
    jsonObjWifi["ip"] = new JSONValue(WiFi.localIP().toString().c_str());

    // data->memory
    JSONObject jsonObjMemory;
    jsonObjMemory["heap_total"] = new JSONValue((int)memGet.getHeapSize());
    jsonObjMemory["heap_free"] = new JSONValue((int)memGet.getFreeHeap());
    jsonObjMemory["psram_total"] = new JSONValue((int)memGet.getPsramSize());
    jsonObjMemory["psram_free"] = new JSONValue((int)memGet.getFreePsram());
    jsonObjMemory["fs_total"] = new JSONValue((int)FSCom.totalBytes());
    jsonObjMemory["fs_used"] = new JSONValue((int)FSCom.usedBytes());
    jsonObjMemory["fs_free"] = new JSONValue(int(FSCom.totalBytes() - FSCom.usedBytes()));

    // data->power
    JSONObject jsonObjPower;
    jsonObjPower["battery_percent"] = new JSONValue(powerStatus->getBatteryChargePercent());
    jsonObjPower["battery_voltage_mv"] = new JSONValue(powerStatus->getBatteryVoltageMv());
    jsonObjPower["has_battery"] = new JSONValue(BoolToString(powerStatus->getHasBattery()));
    jsonObjPower["has_usb"] = new JSONValue(BoolToString(powerStatus->getHasUSB()));
    jsonObjPower["is_charging"] = new JSONValue(BoolToString(powerStatus->getIsCharging()));

    // data->device
    JSONObject jsonObjDevice;
    jsonObjDevice["reboot_counter"] = new JSONValue((int)myNodeInfo.reboot_count);

    // data->radio
    JSONObject jsonObjRadio;
    jsonObjRadio["frequency"] = new JSONValue(RadioLibInterface::instance->getFreq());
    jsonObjRadio["lora_channel"] = new JSONValue((int)RadioLibInterface::instance->getChannelNum() + 1);

    // collect data to inner data object
    JSONObject jsonObjInner;
    jsonObjInner["airtime"] = new JSONValue(jsonObjAirtime);
    jsonObjInner["wifi"] = new JSONValue(jsonObjWifi);
    jsonObjInner["memory"] = new JSONValue(jsonObjMemory);
    jsonObjInner["power"] = new JSONValue(jsonObjPower);
    jsonObjInner["device"] = new JSONValue(jsonObjDevice);
    jsonObjInner["radio"] = new JSONValue(jsonObjRadio);

    // create json output structure
    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(jsonObjInner);
    jsonObjOuter["status"] = new JSONValue("ok");
    // serialize and write it to the stream
    JSONValue *value = new JSONValue(jsonObjOuter);
    res->print(value->Stringify().c_str());
    delete value;
}

/*
This supports the Apple Captive Network Assistant (CNA) Portal
*/
void handleHotspot(HTTPRequest *req, HTTPResponse *res)
{
    LOG_INFO("Hotspot Request\n");

    /*
    If we don't do a redirect, be sure to return a "Success" message
    otherwise iOS will have trouble detecting that the connection to the SoftAP worked.
    */

    // Status code is 200 OK by default.
    // We want to deliver a simple HTML page, so we send a corresponding content type:
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    // res->println("<!DOCTYPE html>");
    res->println("<meta http-equiv=\"refresh\" content=\"0;url=/\" />\n");
}

void handleDeleteFsContent(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>\n");
    res->println("Deleting Content in /static/*");

    LOG_INFO("Deleting files from /static/* : \n");

    htmlDeleteDir("/static");

    res->println("<p><hr><p><a href=/admin>Back to admin</a>\n");
}

void handleAdmin(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>\n");
    res->println("<a href=/json/report>Device Report</a><br>\n");
}

void handleAdminSettings(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>\n");
    res->println("This isn't done.\n");
    res->println("<form action=/admin/settings/apply method=post>\n");
    res->println("<table border=1>\n");
    res->println("<tr><td>Set?</td><td>Setting</td><td>current value</td><td>new value</td></tr>\n");
    res->println("<tr><td><input type=checkbox></td><td>WiFi SSID</td><td>false</td><td><input type=radio></td></tr>\n");
    res->println("<tr><td><input type=checkbox></td><td>WiFi Password</td><td>false</td><td><input type=radio></td></tr>\n");
    res->println(
        "<tr><td><input type=checkbox></td><td>Smart Position Update</td><td>false</td><td><input type=radio></td></tr>\n");
    res->println("</table>\n");
    res->println("<table>\n");
    res->println("<input type=submit value=Apply New Settings>\n");
    res->println("<form>\n");
    res->println("<p><hr><p><a href=/admin>Back to admin</a>\n");
}

void handleAdminSettingsApply(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "POST");
    res->println("<h1>Meshtastic</h1>\n");
    res->println(
        "<html><head><meta http-equiv=\"refresh\" content=\"1;url=/admin/settings\" /><title>Settings Applied. </title>");

    res->println("Settings Applied. Please wait.\n");
}

void handleFs(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>\n");
    res->println("<a href=/admin/fs/delete>Delete Web Content</a><p><form action=/admin/fs/update "
                 "method=post><input type=submit value=UPDATE_WEB_CONTENT></form>Be patient!");
    res->println("<p><hr><p><a href=/admin>Back to admin</a>\n");
}

void handleRestart(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>\n");
    res->println("Restarting");

    LOG_DEBUG("***** Restarted on HTTP(s) Request *****\n");
    webServerThread->requestRestart = (millis() / 1000) + 5;
}

void handleBlinkLED(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "POST");

    ResourceParameters *params = req->getParams();
    std::string blink_target;

    if (!params->getQueryParameter("blink_target", blink_target)) {
        // if no blink_target was supplied in the URL parameters of the
        // POST request, then assume we should blink the LED
        blink_target = "LED";
    }

    if (blink_target == "LED") {
        uint8_t count = 10;
        while (count > 0) {
            setLed(true);
            delay(50);
            setLed(false);
            delay(50);
            count = count - 1;
        }
    } else {
#if HAS_SCREEN
        screen->blink();
#endif
    }

    JSONObject jsonObjOuter;
    jsonObjOuter["status"] = new JSONValue("ok");
    JSONValue *value = new JSONValue(jsonObjOuter);
    res->print(value->Stringify().c_str());
    delete value;
}

void handleScanNetworks(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    int n = WiFi.scanNetworks();

    // build list of network objects
    JSONArray networkObjs;
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            char ssidArray[50];
            String ssidString = String(WiFi.SSID(i));
            ssidString.replace("\"", "\\\"");
            ssidString.toCharArray(ssidArray, 50);

            if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                JSONObject thisNetwork;
                thisNetwork["ssid"] = new JSONValue(ssidArray);
                thisNetwork["rssi"] = new JSONValue(int(WiFi.RSSI(i)));
                networkObjs.push_back(new JSONValue(thisNetwork));
            }
            // Yield some cpu cycles to IP stack.
            // This is important in case the list is large and it takes us time to return
            // to the main loop.
            yield();
        }
    }

    // build output structure
    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(networkObjs);
    jsonObjOuter["status"] = new JSONValue("ok");

    // serialize and write it to the stream
    JSONValue *value = new JSONValue(jsonObjOuter);
    res->print(value->Stringify().c_str());
    delete value;
}

// OTA handler functions
void ota_handleFirmwareUpload(HTTPRequest *req, HTTPResponse *res)
{
    if (req->getMethod() == "POST") {
        auto contentLength = req->getHeader("Content-Length");
        Serial.println("Received OTA update request");

        if (!contentLength.empty()) {
            int len = std::stoi(contentLength);
            Serial.printf("Content-Length: %d\n", len);

            const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
            const esp_partition_t *running_partition = esp_ota_get_running_partition();

            if (update_partition == NULL) {
                Serial.println("Update partition not found");
                res->setStatusCode(500);
                res->setStatusText("Update failed");
                res->println("Update partition not found");
                return;
            }

            Serial.printf("Update partition: type %d, subtype %d, offset 0x%08x, size 0x%08x\n", update_partition->type,
                          update_partition->subtype, update_partition->address, update_partition->size);
            Serial.printf("Running partition: type %d, subtype %d, offset 0x%08x, size 0x%08x\n", running_partition->type,
                          running_partition->subtype, running_partition->address, running_partition->size);

            if (len > update_partition->size) {
                Serial.println("Firmware size is too large for the partition");
                res->setStatusCode(500);
                res->setStatusText("Update failed");
                res->println("Firmware size is too large for the partition");
                return;
            }

            esp_ota_handle_t update_handle = 0;
            esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
            if (err != ESP_OK) {
                Serial.printf("esp_ota_begin failed (%s)\n", esp_err_to_name(err));
                res->setStatusCode(500);
                res->setStatusText("Update failed");
                res->println("OTA begin failed");
                return;
            }

            int written = 0;
            bool header_skipped = false;
            while (written < len) {
                uint8_t buffer[128];
                int bytesRead = req->readBytes(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                    // Skip multipart headers
                    if (!header_skipped) {
                        String headerEnd = "\r\n\r\n";
                        int headerEndIdx = -1;
                        for (int i = 0; i < bytesRead - 3; i++) {
                            if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                                headerEndIdx = i + 3;
                                break;
                            }
                        }
                        if (headerEndIdx != -1) {
                            bytesRead -= (headerEndIdx + 1);
                            memmove(buffer, buffer + headerEndIdx + 1, bytesRead);
                            header_skipped = true;
                        } else {
                            continue; // continue reading until the header is skipped
                        }
                    }

                    err = esp_ota_write_with_offset(update_handle, buffer, bytesRead, written);
                    if (err != ESP_OK) {
                        Serial.printf("esp_ota_write_with_offset failed (%s)\n", esp_err_to_name(err));
                        esp_ota_end(update_handle);
                        res->setStatusCode(500);
                        res->setStatusText("Update failed");
                        res->println("OTA write failed");
                        return;
                    }
                    written += bytesRead;
                    Serial.printf("Written %d bytes so far\n", written);
                } else if (bytesRead == 0) {
                    break; // End of the stream
                } else {
                    Serial.printf("Request read failed with error: %d\n", bytesRead);
                    esp_ota_end(update_handle);
                    res->setStatusCode(500);
                    res->setStatusText("Update failed");
                    res->println("Request read failed");
                    return;
                }
            }

            err = esp_ota_end(update_handle);
            if (err != ESP_OK) {
                Serial.printf("esp_ota_end failed (%s)\n", esp_err_to_name(err));
                res->setStatusCode(500);
                res->setStatusText("Update failed");
                res->println("OTA end failed");
                return;
            }

            err = esp_ota_set_boot_partition(update_partition);
            if (err != ESP_OK) {
                Serial.printf("esp_ota_set_boot_partition failed (%s)\n", esp_err_to_name(err));
                res->setStatusCode(500);
                res->setStatusText("Update failed");
                res->println("Set boot partition failed");
                return;
            }

            Serial.println("OTA update successful, rebooting...");
            res->setStatusCode(200);
            res->setStatusText("Update Success");
            res->println("Update Success! Rebooting...");
            delay(1000);
            ESP.restart();
        } else {
            Serial.println("No Content-Length header received");
            res->setStatusCode(411);
            res->setStatusText("Length Required");
            res->println("No Content-Length header received");
        }
    } else {
        res->setStatusCode(405);
        res->setStatusText("Method Not Allowed");
        res->println("Only POST method is allowed");
    }
}

// Function to serve the OTA upload form
void handleOTAUploadForm(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->println("<form method='POST' action='/admin/update' enctype='multipart/form-data'>"
                 "<input type='file' name='firmware' accept='.bin'>"
                 "<input type='submit' value='Update Firmware'>"
                 "</form>");
}

// Register handlers
void registerHandlers(HTTPServer *insecureServer, HTTPSServer *secureServer)
{
    // OTA update nodes
    ResourceNode *nodeOTAUploadForm = new ResourceNode("/admin/ota", "GET", &handleOTAUploadForm);
    ResourceNode *nodeOTAUpload = new ResourceNode("/admin/update", "POST", &ota_handleFirmwareUpload);
    // Original nodes
    ResourceNode *nodeAPIv1ToRadioOptions = new ResourceNode("/api/v1/toradio", "OPTIONS", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1ToRadio = new ResourceNode("/api/v1/toradio", "PUT", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1FromRadio = new ResourceNode("/api/v1/fromradio", "GET", &handleAPIv1FromRadio);
    ResourceNode *nodeAdmin = new ResourceNode("/admin", "GET", &handleAdmin);
    ResourceNode *nodeRestart = new ResourceNode("/restart", "POST", &handleRestart);
    ResourceNode *nodeFormUpload = new ResourceNode("/upload", "POST", &handleFormUpload);
    ResourceNode *nodeJsonScanNetworks = new ResourceNode("/json/scanNetworks", "GET", &handleScanNetworks);
    ResourceNode *nodeJsonBlinkLED = new ResourceNode("/json/blink", "POST", &handleBlinkLED);
    ResourceNode *nodeJsonReport = new ResourceNode("/json/report", "GET", &handleReport);
    ResourceNode *nodeJsonFsBrowseStatic = new ResourceNode("/json/fs/browse/static", "GET", &handleFsBrowseStatic);
    ResourceNode *nodeJsonDelete = new ResourceNode("/json/fs/delete/static", "DELETE", &handleFsDeleteStatic);
    ResourceNode *nodeRoot = new ResourceNode("/*", "GET", &handleStatic);

    // Secure nodes
    secureServer->registerNode(nodeAPIv1ToRadioOptions);
    secureServer->registerNode(nodeAPIv1ToRadio);
    secureServer->registerNode(nodeAPIv1FromRadio);
    secureServer->registerNode(nodeRestart);
    secureServer->registerNode(nodeFormUpload);
    secureServer->registerNode(nodeJsonScanNetworks);
    secureServer->registerNode(nodeJsonBlinkLED);
    secureServer->registerNode(nodeJsonFsBrowseStatic);
    secureServer->registerNode(nodeJsonDelete);
    secureServer->registerNode(nodeJsonReport);
    secureServer->registerNode(nodeOTAUploadForm);
    secureServer->registerNode(nodeOTAUpload);
    secureServer->registerNode(nodeAdmin);
    secureServer->registerNode(nodeRoot);

    // Insecure nodes
    insecureServer->registerNode(nodeAPIv1ToRadioOptions);
    insecureServer->registerNode(nodeAPIv1ToRadio);
    insecureServer->registerNode(nodeAPIv1FromRadio);
    insecureServer->registerNode(nodeRestart);
    insecureServer->registerNode(nodeFormUpload);
    insecureServer->registerNode(nodeJsonScanNetworks);
    insecureServer->registerNode(nodeJsonBlinkLED);
    insecureServer->registerNode(nodeJsonFsBrowseStatic);
    insecureServer->registerNode(nodeJsonDelete);
    insecureServer->registerNode(nodeJsonReport);
    insecureServer->registerNode(nodeOTAUploadForm);
    insecureServer->registerNode(nodeOTAUpload);
    insecureServer->registerNode(nodeAdmin);
    insecureServer->registerNode(nodeRoot);
}

#endif