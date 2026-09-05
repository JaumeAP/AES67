#include "webconfig.h"

#include <QNEthernet.h>

#include "profiles.h"
#include "wordclock.h"

using qindesign::network::EthernetClient;
using qindesign::network::EthernetServer;

static EthernetServer server(80);
static ProfileApplyFn applyFn = nullptr;
static RateApplyFn applyRateFn = nullptr;

// The longest we wait for the request line of a client that has already
// connected. With no limit, a client that opens the connection and then says
// nothing would leave the box waiting, and here the one thing that must never
// stop is PTP.
static const uint32_t kRequestTimeoutMs = 200;

// The longest request line accepted. All we ever ask for is
// "GET /select?p=2 HTTP/1.1"; anything beyond that is noise or somebody
// trying their luck, and it is discarded without being read.
static const size_t kRequestLineMax = 128;

void webconfigBegin(ProfileApplyFn apply, RateApplyFn applyRate)
{
  applyFn = apply;
  applyRateFn = applyRate;
  server.begin();
}

// Reads the first line of the request. Returns false if it does not arrive
// whole in time or if it goes past kRequestLineMax.
static bool readRequestLine(EthernetClient &client, char *out, size_t outSize)
{
  size_t n = 0;
  const uint32_t deadline = millis() + kRequestTimeoutMs;

  while((int32_t)(millis() - deadline) < 0){
    if(!client.connected()){
      return false;
    }
    if(!client.available()){
      continue;
    }

    const int c = client.read();
    if(c < 0){
      continue;
    }
    if(c == '\n'){
      out[n] = '\0';
      // The line ends in \r\n; the \r is surplus.
      if(n > 0 && out[n - 1] == '\r'){
        out[n - 1] = '\0';
      }
      return true;
    }
    if(n + 1 >= outSize){
      return false;
    }
    out[n++] = (char)c;
  }
  return false;
}

// The rest of the request is read and thrown away. If it is not drained, the
// client can get an RST before it has finished sending and the browser shows
// an error instead of the page.
static void drainRequest(EthernetClient &client)
{
  const uint32_t deadline = millis() + kRequestTimeoutMs;
  while(client.connected() && (int32_t)(millis() - deadline) < 0){
    if(client.available()){
      if(client.read() < 0){
        break;
      }
    }else{
      break;
    }
  }
}

static void sendPage(EthernetClient &client, size_t selected, size_t selectedRate)
{
  client.print("HTTP/1.1 200 OK\r\n"
               "Content-Type: text/html; charset=utf-8\r\n"
               "Connection: close\r\n"
               "\r\n");

  client.print("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>AES67 MasterBox</title>"
               "<style>body{font-family:system-ui,sans-serif;margin:2rem;max-width:40rem}"
               "li{margin:.6rem 0}small{color:#555}</style>"
               "</head><body><h1>PTP profile</h1><form method=\"get\" action=\"/select\"><ul>");

  for(size_t i = 0; i < profileCount(); i++){
    const PtpProfile &p = profileAt(i);
    client.printf("<li><label><input type=\"radio\" name=\"p\" value=\"%u\"%s> "
                  "<b>%s</b><br><small>%s</small><br>"
                  "<small>domain %u &middot; sync log %d &middot; announce log %d "
                  "&middot; priority1 %u &middot; locked &lt; %d ns</small>"
                  "</label></li>",
                  (unsigned)i,
                  i == selected ? " checked" : "",
                  p.name,
                  p.summary,
                  (unsigned)p.domainNumber,
                  (int)p.logSyncInterval,
                  (int)p.logAnnounceInterval,
                  (unsigned)p.priority1,
                  (int)p.lockThresholdNs);
  }

  client.print("</ul><button type=\"submit\">Apply</button></form>");

  client.print("<h1>Word clock rate</h1>"
               "<form method=\"get\" action=\"/rate\"><ul>");

  for(size_t i = 0; i < wordclockRateCount(); i++){
    client.printf("<li><label><input type=\"radio\" name=\"r\" value=\"%u\"%s> "
                  "<b>%lu Hz</b></label></li>",
                  (unsigned)i,
                  i == selectedRate ? " checked" : "",
                  (unsigned long)wordclockRateAt(i));
  }

  client.print("</ul><button type=\"submit\">Apply</button></form>"
               "<p><small>It has to match what the generator puts out. If it does "
               "not, the PPS will not run at 1 Hz and PTP will be off by the same "
               "proportion; nothing is corrected on its own.</small></p>"
               "<p><small>Both changes are stored and applied at once, with no restart.</small></p>"
               "</body></html>");
}

static void sendRedirect(EthernetClient &client)
{
  client.print("HTTP/1.1 303 See Other\r\n"
               "Location: /\r\n"
               "Connection: close\r\n"
               "\r\n");
}

static void sendNotFound(EthernetClient &client)
{
  client.print("HTTP/1.1 404 Not Found\r\n"
               "Content-Type: text/plain; charset=utf-8\r\n"
               "Connection: close\r\n"
               "\r\n"
               "Only / is served here\n");
}

// Pulls the number out of "GET /select?p=2 HTTP/1.1", with key "?p=" and the
// count of what is being picked. Returns false if it is not there, is not a
// number, or falls outside the list: what comes off the network is never
// assumed good.
static bool parseSelection(const char *line, const char *key, size_t count,
                           size_t &index)
{
  const char *q = strstr(line, key);
  if(q == nullptr){
    return false;
  }
  q += strlen(key);
  if(*q < '0' || *q > '9'){
    return false;
  }

  size_t value = 0;
  while(*q >= '0' && *q <= '9'){
    value = value * 10 + (size_t)(*q - '0');
    if(value > count){
      return false;
    }
    q++;
  }
  if(value >= count){
    return false;
  }

  index = value;
  return true;
}

void webconfigUpdate()
{
  EthernetClient client = server.accept();
  if(!client){
    return;
  }

  char line[kRequestLineMax];
  if(!readRequestLine(client, line, sizeof(line))){
    client.stop();
    return;
  }

  if(strncmp(line, "GET ", 4) != 0){
    drainRequest(client);
    sendNotFound(client);
    client.stop();
    return;
  }

  const char *path = line + 4;

  if(strncmp(path, "/select?", 8) == 0){
    size_t index = 0;
    if(parseSelection(path, "?p=", profileCount(), index) &&
       profileSaveSelection(index)){
      if(applyFn != nullptr){
        applyFn(index);
      }
      Serial.printf("[Web] profile changed to %s\n", profileAt(index).id);
    }else{
      Serial.println("[Web] profile change request discarded");
    }
    drainRequest(client);
    sendRedirect(client);
    client.stop();
    return;
  }

  if(strncmp(path, "/rate?", 6) == 0){
    size_t index = 0;
    if(parseSelection(path, "?r=", wordclockRateCount(), index) &&
       wordclockRateSaveSelection(index)){
      if(applyRateFn != nullptr){
        applyRateFn(index);
      }
      Serial.printf("[Web] word clock rate changed to %lu Hz\n",
                    (unsigned long)wordclockRateAt(index));
    }else{
      Serial.println("[Web] word clock rate change request discarded");
    }
    drainRequest(client);
    sendRedirect(client);
    client.stop();
    return;
  }

  if(strncmp(path, "/ ", 2) == 0 || strncmp(path, "/?", 2) == 0){
    drainRequest(client);
    sendPage(client, profileLoadSelection(), wordclockRateLoadSelection());
    client.stop();
    return;
  }

  drainRequest(client);
  sendNotFound(client);
  client.stop();
}
