/*
    Haptic Pattern Designer Web Server
    74HC595 Shift Register with manual shifting
    ULN2803 Darlington Array driven motors
    binary websocket state broadcasts
    pattern playback engine (non-blocking)
    web UI
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#define DATA_PIN 1
#define CLOCK_PIN 4
#define LATCH_PIN 3

const char *ssid = "YOUR_SSID_HERE";
const char *password = "YOUR_PASSWORD_HERE";

WebServer server(80);
WebSocketsServer ws(81);

uint8_t state = 0;
unsigned long wifiRetry = 0;
bool wifiConnected = false;

/* ---------- Pattern playback ---------- */
const int MAX_STEPS = 64;
uint8_t patMasks[MAX_STEPS];
uint16_t patDur[MAX_STEPS];
int patLen = 0;

bool playing = false;
int playIndex = 0;
unsigned long stepStart = 0;

/* ---------- Per-motor tap timers ---------- */
unsigned long motorTimer[8] = {0};
uint16_t tapDuration = 100;

/* ---------- Helpers ---------- */
uint8_t rev(uint8_t b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

void shift(uint8_t d)
{
    d = rev(d);
    digitalWrite(LATCH_PIN, LOW);
    for (int i = 7; i >= 0; i--)
    {
        digitalWrite(CLOCK_PIN, LOW);
        digitalWrite(DATA_PIN, (d >> i) & 1);
        digitalWrite(CLOCK_PIN, HIGH);
    }
    digitalWrite(LATCH_PIN, HIGH);
}

/* ---------- Motor actions ---------- */
void setState(uint8_t mask)
{
    state = mask;
    shift(state);
    ws.broadcastBIN(&state, 1);
}

void motorTap(uint8_t m)
{
    state |= (1 << m);
    motorTimer[m] = millis();
    shift(state);
    ws.broadcastBIN(&state, 1);
}

void allOff()
{
    state = 0;
    shift(state);
    ws.broadcastBIN(&state, 1);
}

/* ---------- Pattern engine (non-blocking) ---------- */
void startPattern(int len)
{
    if (len <= 0)
        return;
    patLen = (len > MAX_STEPS) ? MAX_STEPS : len;
    playing = true;
    playIndex = 0;

    memset(motorTimer, 0, sizeof(motorTimer));

    stepStart = millis();
    setState(patMasks[playIndex]);
}

void stopPattern()
{
    playing = false;
    patLen = 0;
    playIndex = 0;
    allOff();
}

void updatePattern()
{
    if (!playing || patLen == 0)
        return;

    unsigned long now = millis();
    uint16_t dur = patDur[playIndex];

    if (now - stepStart >= dur)
    {
        playIndex++;
        if (playIndex >= patLen)
        {
            stopPattern();
            return;
        }
        stepStart = now;
        setState(patMasks[playIndex]);
    }
}

/* ---------- WebSocket handling ---------- */
void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
    if (type == WStype_CONNECTED)
    {
        ws.sendBIN(num, &state, 1);
        return;
    }

    if (type == WStype_DISCONNECTED)
    {
        if (ws.connectedClients() == 0)
            stopPattern();
        return;
    }

    if (type == WStype_TEXT)
    {
        if (length >= 1)
        {
            char c = payload[0];
            if (c >= '0' && c <= '7')
            {
                motorTap(c - '0');
            }
            else if (c == 'S')
            {
                stopPattern();
            }
            else if (c == 'T' && length > 1)
            {
                String tStr = (char *)(payload + 1);
                int newT = tStr.toInt();
                if (newT > 0)
                    tapDuration = newT;
            }
        }
        return;
    }

    if (type == WStype_BIN)
    {
        if (length >= 2 && payload[0] == 'P')
        {
            uint8_t steps = payload[1];
            if (steps > MAX_STEPS)
                steps = MAX_STEPS;

            size_t expected = 2 + size_t(steps) * 3;
            if (length < expected)
                return;

            for (uint8_t i = 0; i < steps; i++)
            {
                patMasks[i] = payload[2 + i * 3];
                patDur[i] = (payload[2 + i * 3 + 1] << 8) | payload[2 + i * 3 + 2];
            }
            startPattern(steps);
        }
        return;
    }
}

/* ---------- Web Page (PROGMEM) ---------- */
const char page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no,maximum-scale=1">
<title>Haptic OS</title>
<style>
:root{
  --bg:#09090b;
  --card:#121316;
  --muted:#9aa0a6;
  --accent:#FFC700;
  --on:#FFC700;
  --border:#26282d;
  --radius-lg: 20px;
  --radius-md: 12px;
  --radius-sm: 8px;
}
body{margin:0;font-family:system-ui,sans-serif;background:var(--bg);color:#eee;-webkit-font-smoothing:antialiased;}
.app{padding:16px;max-width:640px;margin:0 auto;}
.btn{background:var(--card);border-radius:var(--radius-md);padding:12px 16px;color:#eee;border:1px solid var(--border);cursor:pointer;font-weight:700;font-size:14px;transition:0.15s ease;}
.btn:active{transform:scale(0.96);opacity:0.8;}
.small{padding:10px 14px;font-size:13px;}
.card{background:var(--card);padding:16px;border-radius:var(--radius-lg);margin-bottom:16px;border:1px solid var(--border);box-shadow: 0 4px 20px rgba(0,0,0,0.2);}
.grid-main{display:grid;grid-template-rows:repeat(4,1fr);grid-auto-flow:column;gap:12px;margin-top:12px;}
.motor{padding:16px 0;border-radius:var(--radius-md);background:#1a1b1f;display:flex;align-items:center;justify-content:center;font-weight:800;color:#777;touch-action:manipulation;font-size:22px;border:2px solid transparent;transition: 0.2s;}
.motor.on{background:linear-gradient(145deg,var(--on),#d4af37);color:#000;box-shadow:0 0 16px rgba(255,199,0,0.25);border-color:#fff;}
.label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:0.8px;font-weight:700;}

/* Sequence Grids */
.steps{display:flex;flex-direction:column;gap:10px;}
.step{background:#090a0c;padding:12px;border-radius:var(--radius-md);border:1px solid #1a1b1f;}
.step-grid{display:grid;grid-template-columns:52px 1fr 1fr 56px;gap:12px;align-items:stretch;}

.mini-grid{display:grid;grid-template-rows:repeat(4,1fr);grid-auto-flow:column;gap:4px;}
.mini-motor{width:24px;height:24px;border-radius:var(--radius-sm);background:#1a1b1f;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:11px;cursor:pointer;color:#777;}
.mini-motor.on{background:var(--accent);color:#000;}

/* Global Spinners (Horizontal) */
.spin-ctrl{display:flex;align-items:center;background:#000;border:1px solid var(--border);border-radius:var(--radius-md);overflow:hidden;height:40px;flex:1;}
.spin-btn{flex:1;min-width:32px;display:flex;align-items:center;justify-content:center;background:#1a1b1f;color:#ccc;font-weight:bold;cursor:pointer;user-select:none;font-size:22px;}
.spin-btn:active{background:var(--accent);color:#000;}
.spin-input{width:50px;background:none;border:none;color:#eee;text-align:center;font-weight:700;font-size:14px;-moz-appearance:textfield;padding:0;}
.spin-input::-webkit-outer-spin-button, .spin-input::-webkit-inner-spin-button {-webkit-appearance:none;margin:0;}

/* Step Spinners (Vertical stretched) */
.spin-ctrl-vert{display:flex;flex-direction:column;background:#000;border:1px solid var(--border);border-radius:var(--radius-md);overflow:hidden;width:100%;}
.spin-btn-vert{flex:1;display:flex;align-items:center;justify-content:center;background:#1a1b1f;color:#ccc;font-weight:bold;cursor:pointer;user-select:none;font-size:20px;}
.spin-btn-vert:active{background:var(--accent);color:#000;}
.spin-input-vert{height:34px;background:none;border:none;border-top:1px solid var(--border);border-bottom:1px solid var(--border);color:#eee;text-align:center;font-weight:700;font-size:13px;padding:0;-moz-appearance:textfield;}
.spin-input-vert::-webkit-outer-spin-button, .spin-input-vert::-webkit-inner-spin-button {-webkit-appearance:none;margin:0;}

/* Step Controls (Vertical stretched) */
.step-controls{display:flex;flex-direction:column;gap:8px;width:100%;}
.step-controls .btn{flex:1;display:flex;align-items:center;justify-content:center;margin:0;padding:0;font-size:12px;border-radius:var(--radius-md);}

.lib-list{display:flex;flex-direction:column;gap:10px;margin-top:12px;}
.lib-item{display:flex;justify-content:space-between;align-items:center;background:#090a0c;padding:12px 14px;border-radius:var(--radius-md);border:1px solid #1a1b1f;}
.footer{display:flex;flex-direction:column;gap:12px;margin-top:8px;padding-bottom:32px;}
.footer-top{display:flex;justify-content:space-between;align-items:center;}
.notice{font-size:13px;color:var(--muted);font-weight:600;}
.accent-btn{background:var(--accent);color:#000;border:none;box-shadow: 0 4px 12px rgba(255,199,0,0.2);}
.config-bar{background:#090a0c;border-radius:var(--radius-md);padding:12px;display:flex;gap:14px;border:1px solid #1a1b1f;margin-bottom:14px;}
.config-col{flex:1;display:flex;flex-direction:column;gap:8px;}
.header-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;}
</style>
</head>
<body>
<div class="app">

  <div class="card">
    <div class="header-row">
      <div class="label">Live Setup</div>
      <div style="display:flex;gap:10px;flex:1;justify-content:flex-end;">
        <button id="testLiveBtn" class="btn small" style="flex:1;max-width:90px;">Test</button>
        <button id="addStep" class="btn small accent-btn" style="flex:1;max-width:90px;">+ Step</button>
      </div>
    </div>
    <div id="motorGrid" class="grid-main"></div>
  </div>

  <div class="card">
    <div class="header-row">
      <div class="label">Composer Sequence</div>
      <button id="clearAll" class="btn small">Clear All</button>
    </div>
    
    <div class="config-bar">
      <div class="config-col">
        <span class="label">Buzz Defaults</span>
        <div class="spin-ctrl">
          <div class="spin-btn" id="buzzDec">-</div>
          <input id="globBuzz" class="spin-input" type="number" min="10" value="100">
          <div class="spin-btn" id="buzzInc">+</div>
        </div>
      </div>
      <div class="config-col">
        <span class="label">Gap Defaults</span>
        <div class="spin-ctrl">
          <div class="spin-btn" id="gapDec">-</div>
          <input id="globGap" class="spin-input" type="number" min="0" value="50">
          <div class="spin-btn" id="gapInc">+</div>
        </div>
      </div>
    </div>

    <div class="step-grid" id="stepHeaders" style="display:none; padding:0 12px; margin-bottom:8px;">
      <div class="label" style="width:100%;text-align:center;">Mask</div>
      <div class="label" style="width:100%;text-align:center;">Buzz</div>
      <div class="label" style="width:100%;text-align:center;">Gap</div>
      <div class="label" style="width:100%;text-align:center;">Opt</div>
    </div>
    <div id="steps" class="steps"></div>
  </div>

  <div class="card">
    <div class="header-row" style="margin-bottom:0;">
      <div class="label">Library</div>
      <button id="saveLib" class="btn small">Save Sequence</button>
    </div>
    <div id="lib" class="lib-list"></div>
  </div>

  <div class="footer">
    <div class="footer-top">
      <div class="notice">Status: <span id="status" style="color:#ff5555">Offline</span></div>
    </div>
    <button id="sendBtn" class="btn accent-btn" style="width:100%; padding:16px; font-size:16px; border-radius:var(--radius-lg);">Deploy Pattern to Device</button>
  </div>
</div>

<script>
(() => {
  const ws = new WebSocket("ws://"+location.hostname+":81/");
  ws.binaryType = "arraybuffer";
  
  const el = (id) => document.getElementById(id);
  const motorGrid = el("motorGrid"), stepsEl = el("steps"), libEl = el("lib");
  const status = el("status"), stepHeaders = el("stepHeaders");
  
  const motorCount = 8;
  let currentSelection = 0; 
  let steps = []; 

  const loadLib = () => JSON.parse(localStorage.getItem('haptic_v9') || '[]');
  const saveLib = (arr) => localStorage.setItem('haptic_v9', JSON.stringify(arr));

  const updateGlob = (id, delta) => {
    const inp = el(id);
    const min = id === 'globBuzz' ? 10 : 0;
    const val = Math.max(min, parseInt(inp.value || 0, 10) + delta);
    inp.value = val;
    if(id === 'globBuzz' && ws && ws.readyState === WebSocket.OPEN) {
      ws.send("T" + val);
    }
  };

  el("buzzDec").onclick = () => updateGlob('globBuzz', -10);
  el("buzzInc").onclick = () => updateGlob('globBuzz', 10);
  el("gapDec").onclick = () => updateGlob('globGap', -10);
  el("gapInc").onclick = () => updateGlob('globGap', 10);
  el("globBuzz").onchange = () => updateGlob('globBuzz', 0);
  el("globGap").onchange = () => updateGlob('globGap', 0);

  const getGlobBuzz = () => Math.max(10, parseInt(el("globBuzz").value, 10));
  const getGlobGap = () => Math.max(0, parseInt(el("globGap").value, 10));

  const renderGrid = () => {
    motorGrid.innerHTML = '';
    Array.from({length: motorCount}).forEach((_, i) => {
      const b = document.createElement('div');
      b.className = `motor ${(currentSelection & (1<<i)) ? 'on' : ''}`;
      b.innerText = (i + 1);
      b.onclick = () => {
        currentSelection ^= (1 << i);
        renderGrid();
      };
      motorGrid.appendChild(b);
    });
  };

  const createVertSpinner = (val, min, onChange) => {
    const wrap = document.createElement('div'); wrap.className = 'spin-ctrl-vert';
    const inc = document.createElement('div'); inc.className = 'spin-btn-vert'; inc.innerText = '+';
    const inp = document.createElement('input'); inp.className = 'spin-input-vert'; inp.type = 'number'; inp.value = val;
    const dec = document.createElement('div'); dec.className = 'spin-btn-vert'; dec.innerText = '-';
    
    const update = (newVal) => {
      val = Math.max(min, newVal);
      inp.value = val;
      onChange(val);
    };
    inc.onclick = () => update(val + 10);
    dec.onclick = () => update(val - 10);
    inp.onchange = (e) => update(parseInt(e.target.value||min, 10));
    
    wrap.append(inc, inp, dec);
    return wrap;
  };

  const renderSteps = () => {
    if(!steps.length) {
      stepHeaders.style.display = 'none';
      stepsEl.innerHTML = '<div class="notice" style="text-align:center;padding:20px 0;opacity:0.5;">Sequence is empty</div>';
      return;
    }
    stepHeaders.style.display = 'grid';
    stepsEl.innerHTML = '';
    
    steps.forEach((s, idx) => {
      const row = document.createElement('div'); 
      row.className = 'step step-grid';
      
      const miniGrid = document.createElement('div'); 
      miniGrid.className = 'mini-grid';
      Array.from({length: motorCount}).forEach((_, i) => {
        const isOn = (s.mask >> i) & 1;
        const c = document.createElement('div');
        c.className = `mini-motor ${isOn ? 'on' : ''}`;
        c.innerText = i + 1;
        c.onclick = () => { s.mask ^= (1<<i); renderSteps(); };
        miniGrid.appendChild(c);
      });

      const buzzSpin = createVertSpinner(s.buzz, 10, (v) => s.buzz = v);
      const gapSpin = createVertSpinner(s.gap, 0, (v) => s.gap = v);

      const controls = document.createElement('div'); 
      controls.className = 'step-controls';
      
      const up = document.createElement('button'); up.className = 'btn'; up.innerText = 'Up';
      up.onclick = () => { if(idx > 0){ steps.splice(idx-1, 0, steps.splice(idx,1)[0]); renderSteps(); }};
      
      const del = document.createElement('button'); del.className = 'btn'; del.innerText = 'Del';
      del.onclick = () => { steps.splice(idx, 1); renderSteps(); };

      controls.append(up, del);
      row.append(miniGrid, buzzSpin, gapSpin, controls);
      stepsEl.appendChild(row);
    });
  };

  const renderLib = () => {
    libEl.innerHTML = '';
    const libData = loadLib();
    if(!libData.length) {
      libEl.innerHTML = '<div class="notice" style="text-align:center;padding:16px 0;opacity:0.5;">No saved sequences</div>';
      return;
    }
    libData.forEach((p, i) => {
      const r = document.createElement('div'); r.className = 'lib-item';
      const name = document.createElement('div'); name.innerText = p.name || `Seq ${i+1}`;
      name.style.fontWeight = '700'; name.style.fontSize = '14px';
      
      const actions = document.createElement('div'); 
      actions.style.cssText = 'display:flex;gap:10px';
      
      const edit = document.createElement('button'); edit.className = 'btn small'; edit.innerText = 'Load';
      edit.onclick = () => { steps = JSON.parse(JSON.stringify(p.steps)); renderSteps(); };
      
      const remove = document.createElement('button'); remove.className = 'btn small'; remove.innerText = 'Del';
      remove.onclick = () => { const a = loadLib(); a.splice(i, 1); saveLib(a); renderLib(); };
      
      actions.append(edit, remove);
      r.append(name, actions);
      libEl.appendChild(r);
    });
  };

  el("addStep").onclick = () => {
    steps.push({mask: currentSelection, buzz: getGlobBuzz(), gap: getGlobGap()});
    currentSelection = 0;
    renderGrid(); renderSteps();
  };

  el("testLiveBtn").onclick = () => {
    if(!ws || ws.readyState !== WebSocket.OPEN) return alert('Hardware offline');
    if(currentSelection === 0) return; 
    
    const dur = getGlobBuzz();
    const buf = new ArrayBuffer(5);
    const v = new Uint8Array(buf);
    v[0] = 0x50; // 'P'
    v[1] = 1;    // 1 step
    v[2] = currentSelection;
    v[3] = (dur >> 8) & 0xFF;
    v[4] = dur & 0xFF;
    ws.send(buf);
  };

  el("clearAll").onclick = () => { steps = []; renderSteps(); };

  el("sendBtn").onclick = () => {
    if(!ws || ws.readyState !== WebSocket.OPEN) { alert('Hardware disconnected'); return; }
    if(!steps.length) { ws.send("S"); return; } 
    
    let hwSteps = [];
    steps.forEach(s => {
      hwSteps.push({mask: s.mask, dur: s.buzz});
      if(s.gap > 0) hwSteps.push({mask: 0, dur: s.gap});
    });
    
    const maxSteps = hwSteps.slice(0, 64);
    const buf = new ArrayBuffer(2 + maxSteps.length * 3);
    const v = new Uint8Array(buf);
    v[0] = 0x50; // 'P'
    v[1] = maxSteps.length;
    
    maxSteps.forEach((s, i) => {
      v[2 + i*3] = s.mask;
      v[2 + i*3 + 1] = (s.dur >> 8) & 0xFF;
      v[2 + i*3 + 2] = s.dur & 0xFF;
    });
    ws.send(buf);
  };

  el("saveLib").onclick = () => {
    if(!steps.length) return alert('Add steps first');
    const name = prompt('Name this sequence', 'Seq ' + Date.now().toString().slice(-4));
    if(!name) return;
    const arr = loadLib();
    arr.push({name, steps: JSON.parse(JSON.stringify(steps))});
    saveLib(arr); renderLib();
  };

  ws.onopen = () => { 
    status.innerText = 'Online'; 
    status.style.color = 'var(--on)';
    ws.send("T" + getGlobBuzz()); 
  };
  ws.onclose = () => { status.innerText = 'Offline'; status.style.color = '#ff5555'; };
  ws.onerror = () => { status.innerText = 'Error'; status.style.color = '#ff5555'; };
  ws.onmessage = (ev) => {};

  renderGrid(); renderSteps(); renderLib();
})();
</script>
</body>
</html>
)rawliteral";

/* ---------- WiFi ---------- */
void connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return;
    if (millis() - wifiRetry < 5000)
        return;

    wifiRetry = millis();
    WiFi.disconnect();
    WiFi.begin(ssid, password);
}

/* ---------- setup / loop ---------- */
void setup()
{
    Serial.begin(115200);

    pinMode(DATA_PIN, OUTPUT);
    pinMode(CLOCK_PIN, OUTPUT);
    pinMode(LATCH_PIN, OUTPUT);

    allOff();

    WiFi.mode(WIFI_STA);
    connectWiFi();

    server.on("/", [](){ server.send_P(200, "text/html", page); });
    server.begin();

    ws.begin();
    ws.onEvent(wsEvent);
}

void loop()
{
    bool now = (WiFi.status() == WL_CONNECTED);
    if (!now)
    {
        allOff();
        connectWiFi();
        wifiConnected = false;
    }
    else if (!wifiConnected)
    {
        wifiConnected = true;
        Serial.print("Webserver IP: ");
        Serial.println(WiFi.localIP());
    }

    if (!playing)
    {
        unsigned long tnow = millis();
        bool changed = false;
        for (int i = 0; i < 8; i++)
        {
            if ((state & (1 << i)) && motorTimer[i] != 0 && (tnow - motorTimer[i] >= tapDuration))
            {
                state &= ~(1 << i);
                motorTimer[i] = 0;
                changed = true;
            }
        }
        if (changed)
        {
            shift(state);
            ws.broadcastBIN(&state, 1);
        }
    }

    updatePattern();

    server.handleClient();
    ws.loop();
}