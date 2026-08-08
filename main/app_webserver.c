/**
 * @file app_webserver.c
 * @brief Реализация HTTP веб-сервера с защитой HTTP Basic Auth, логами и безопасным OTA обновлением.
 */

#include "app_webserver.h"
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_sdcard.h"
#include "app_gnss.h"
#include "app_log_buffer.h"
#include "app_buzzer.h"
#include "app_ota.h"
#include "app_wifi.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

// ====================================================================================
static const char *BUILD_DATETIME = __DATE__ " " __TIME__;

system_checklist_t g_system_checklist = {
    .nvs_config_ok = false,
    .sd_card_ok = false,
    .gnss_ok = false,
    .wifi_ok = false
};

// ====================================================================================
// Встроенный HTML шаблон веб-интерфейса
// ====================================================================================
static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
"<title>ESP32 GNSS Control</title><style>"
":root{--bg:#0f172a;--card:#1e293b;--primary:#38bdf8;--text:#f8fafc;--sub:#94a3b8;--border:#334155;--ok:#22c55e;--err:#ef4444;--warn:#eab308;}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:sans-serif;}"
"body{background:var(--bg);color:var(--text);padding:15px;max-width:850px;margin:0 auto;}"
".header{display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;}"
"h1{font-size:1.3rem;color:var(--primary);}"
".nav{display:flex;gap:8px;margin-bottom:15px;border-bottom:1px solid var(--border);padding-bottom:8px;}"
".nav button{background:none;border:none;color:var(--sub);font-size:0.95rem;padding:6px 14px;cursor:pointer;border-radius:6px;}"
".nav button.active{background:var(--card);color:var(--primary);font-weight:bold;}"
".tab{display:none;}.tab.active{display:block;}"
".card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:16px;margin-bottom:15px;}"
".banner-err{background:rgba(239,68,68,0.15);border:1px solid var(--err);color:#fca5a5;padding:12px;border-radius:8px;margin-bottom:15px;font-weight:bold;display:none;}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;}"
".stat{background:rgba(15,23,42,0.6);padding:10px;border-radius:6px;border:1px solid var(--border);}"
".stat-label{font-size:0.75rem;color:var(--sub);margin-bottom:2px;}"
".stat-val{font-size:0.95rem;font-weight:bold;}"
".dropzone{border:2px dashed var(--primary);border-radius:8px;padding:25px;text-align:center;cursor:pointer;background:rgba(56,189,248,0.05);margin-bottom:12px;}"
".dropzone:hover{background:rgba(56,189,248,0.1);}"
".pbar{width:100%;height:10px;background:var(--border);border-radius:5px;overflow:hidden;margin-top:10px;display:none;}"
".pfill{height:100%;background:var(--primary);width:0%;transition:width 0.2s;}"
".btn{background:var(--primary);color:#000;font-weight:bold;border:none;padding:8px 16px;border-radius:6px;cursor:pointer;}"
".btn-danger{background:var(--err);color:#fff;}"
".btn:disabled{opacity:0.5;cursor:not-allowed;}"
".logbox{background:#020617;border:1px solid var(--border);border-radius:6px;padding:10px;font-family:monospace;font-size:0.8rem;height:320px;overflow-y:auto;white-space:pre-wrap;color:#38bdf8;}"
".badge-ok{color:var(--ok);font-weight:bold;}.badge-err{color:var(--err);font-weight:bold;}"
".cfg-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}"
".cfg-field{display:flex;flex-direction:column;gap:4px;}"
".cfg-field label{font-size:0.8rem;color:var(--sub);}"
".cfg-field input,.cfg-field select{background:#020617;border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:6px;font-size:0.9rem;}"
".cfg-field input:disabled{opacity:0.6;cursor:not-allowed;background:rgba(255,255,255,0.05);}"
".badge-default{color:var(--ok);font-size:0.75rem;font-weight:normal;margin-left:4px;}"
"</style></head><body>"
"<div class=\"header\"><h1>🛰 ESP32 GNSS Control</h1><button class=\"btn btn-danger\" onclick=\"rb()\">🔄 Перезагрузить</button></div>"
"<div id=\"warn_banner\" class=\"banner-err\">⚠️ Внимание! Обнаружена ошибка оборудования (SD/GNSS). Запись треков остановлена. Используйте Web-интерфейс для диагностики и обновления.</div>"
"<div id=\"wardriving_banner\" class=\"banner-err\" style=\"background:rgba(234,179,8,0.12);border-color:var(--warn);color:#fef08a;\">ℹ️ WiFi-сканирование (Wardriving) отключено в режиме веб-сервера. Запись GPX-треков активна.</div>"
"<div class=\"nav\">"
"<button class=\"active\" onclick=\"t('dash',this)\">Дашборд & Чек-лист</button>"
"<button onclick=\"t('cfg',this)\">⚙️ Конфигурация</button>"
"<button onclick=\"t('ota',this)\">Обновление ПО (OTA)</button>"
"<button onclick=\"t('logs',this)\">Системный лог</button>"
"</div>"
"<div id=\"dash\" class=\"tab active\">"
"<div class=\"card\">"
"<h3 style=\"margin-bottom:10px;color:var(--primary);\">📋 Чек-лист диагностики оборудования</h3>"
"<div class=\"grid\">"
"<div class=\"stat\"><div class=\"stat-label\">1. Конфигурация NVS</div><div class=\"stat-val\" id=\"chk_nvs\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">2. Подключение Wi-Fi AP</div><div class=\"stat-val\" id=\"chk_wifi\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">3. SD-Карта</div><div class=\"stat-val\" id=\"chk_sd\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">4. GNSS Модуль</div><div class=\"stat-val\" id=\"chk_gnss\">-</div></div>"
"</div></div>"
"<div class=\"card\">"
"<h3 style=\"margin-bottom:10px;color:var(--primary);\">📊 Параметры системы</h3>"
"<div class=\"grid\">"
"<div class=\"stat\"><div class=\"stat-label\">Дата и время сборки</div><div class=\"stat-val\" id=\"bld\" style=\"font-size:0.85rem;color:var(--primary);\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Версия прошивки</div><div class=\"stat-val\" id=\"fwv\" style=\"font-size:0.85rem;color:var(--primary);\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Свободная память</div><div class=\"stat-val\" id=\"hp\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Время работы</div><div class=\"stat-val\" id=\"ut\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Причина перезагрузки</div><div class=\"stat-val\" id=\"rr\" style=\"font-size:0.85rem;\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">GNSS 3D Fix</div><div class=\"stat-val\" id=\"fix\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Координаты</div><div class=\"stat-val\" id=\"pos\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Спутники</div><div class=\"stat-val\" id=\"sat\">-</div></div>"
"</div></div></div>"
"<div id=\"cfg\" class=\"tab\"><div class=\"card\">"
"<h3 style=\"margin-bottom:12px;color:var(--primary);\">⚙️ Редактор параметров конфигурации</h3>"
"<p style=\"font-size:0.85rem;color:var(--sub);margin-bottom:15px;\">"
"Параметры, совпадающие с дефолтными, помечены <span class=\"badge-default\">[По умолчанию]</span>. Параметры SSID, FTP Addr, FTP User и Web User заблокированы. Изменения сохраняются только в файл <code>/sdcard/config.json</code> и применятся после перезагрузки."
"</p>"
"<h4 style=\"color:var(--primary);margin:10px 0 8px 0;\">📶 Параметры Wi-Fi</h4>"
"<div class=\"cfg-grid\">"
"<div class=\"cfg-field\"><label>SSID точки (Read-Only) <span id=\"def_ssid\" class=\"badge-default\"></span></label><input type=\"text\" id=\"c_ssid\" disabled maxlength=\"31\"></div>"
"<div class=\"cfg-field\"><label>Пароль Wi-Fi (max 63) <span id=\"def_wifipasswd\" class=\"badge-default\"></span></label><input type=\"password\" id=\"c_wifipasswd\" maxlength=\"63\" placeholder=\"••••••••\" oninput=\"cp('wifipasswd')\"></div>"
"<div class=\"cfg-field\" id=\"w_wifipasswd_c\" style=\"display:none;\"><label style=\"color:var(--warn);\">Повторите пароль Wi-Fi</label><input type=\"password\" id=\"c_wifipasswd_c\" maxlength=\"63\" placeholder=\"Повторите пароль\"></div>"
"</div>"
"<h4 style=\"color:var(--primary);margin:15px 0 8px 0;\">🌐 Параметры FTP</h4>"
"<div class=\"cfg-grid\">"
"<div class=\"cfg-field\"><label>FTP Сервер (Read-Only) <span id=\"def_ftpaddress\" class=\"badge-default\"></span></label><input type=\"text\" id=\"c_ftpaddress\" disabled maxlength=\"63\"></div>"
"<div class=\"cfg-field\"><label>FTP User (Read-Only) <span id=\"def_ftpuser\" class=\"badge-default\"></span></label><input type=\"text\" id=\"c_ftpuser\" disabled maxlength=\"31\"></div>"
"<div class=\"cfg-field\"><label>FTP Пароль (max 31) <span id=\"def_ftppassword\" class=\"badge-default\"></span></label><input type=\"password\" id=\"c_ftppassword\" maxlength=\"31\" placeholder=\"••••••••\" oninput=\"cp('ftppassword')\"></div>"
"<div class=\"cfg-field\" id=\"w_ftppassword_c\" style=\"display:none;\"><label style=\"color:var(--warn);\">Повторите FTP пароль</label><input type=\"password\" id=\"c_ftppassword_c\" maxlength=\"31\" placeholder=\"Повторите пароль\"></div>"
"<div class=\"cfg-field\"><label>FTP Порт (1..65535) <span id=\"def_ftpport\" class=\"badge-default\"></span></label><input type=\"number\" id=\"c_ftpport\" min=\"1\" max=\"65535\"></div>"
"<div class=\"cfg-field\"><label>Лимит файла CSV (байты) <span id=\"def_filesize\" class=\"badge-default\"></span></label><input type=\"number\" id=\"c_filesize\" min=\"1\" max=\"4294967295\"></div>"
"<div class=\"cfg-field\"><label>Отправка GPS треков <span id=\"def_gpssend\" class=\"badge-default\"></span></label><select id=\"c_gpssend\"><option value=\"true\">true</option><option value=\"false\">false</option></select></div>"
"<div class=\"cfg-field\"><label>Отправка Wi-Fi логов <span id=\"def_wifisend\" class=\"badge-default\"></span></label><select id=\"c_wifisend\"><option value=\"true\">true</option><option value=\"false\">false</option></select></div>"
"<div class=\"cfg-field\"><label>Звук по завершению FTP <span id=\"def_ftpbeep\" class=\"badge-default\"></span></label><select id=\"c_ftpbeep\"><option value=\"true\">true</option><option value=\"false\">false</option></select></div>"
"</div>"
"<h4 style=\"color:var(--primary);margin:15px 0 8px 0;\">📡 GNSS и Устройство</h4>"
"<div class=\"cfg-grid\">"
"<div class=\"cfg-field\"><label>ID Модуля (max 7) <span id=\"def_moduleid\" class=\"badge-default\"></span></label><input type=\"text\" id=\"c_moduleid\" maxlength=\"7\"></div>"
"<div class=\"cfg-field\"><label>PACC Mask (в метрах) <span id=\"def_paccmask\" class=\"badge-default\"></span></label><input type=\"number\" id=\"c_paccmask\" min=\"0\" max=\"65535\"></div>"
"<div class=\"cfg-field\"><label>PDOP Mask (*10) <span id=\"def_pdopmask\" class=\"badge-default\"></span></label><input type=\"number\" id=\"c_pdopmask\" min=\"0\" max=\"65535\"></div>"
"<div class=\"cfg-field\"><label>Часовой пояс (UTC offset, ч) <span id=\"def_timezone\" class=\"badge-default\"></span></label><input type=\"number\" id=\"c_timezone\" min=\"-12\" max=\"14\"></div>"
"<div class=\"cfg-field\"><label>Мин. размер файла .wk (байты) <span id=\"def_mintracksize\" class=\"badge-default\"></span></label><input type=\"number\" id=\"c_mintracksize\" min=\"100\" max=\"10485760\"></div>"
"</div>"
"<h4 style=\"color:var(--primary);margin:15px 0 8px 0;\">🔒 Веб-сервер</h4>"
"<div class=\"cfg-grid\">"
"<div class=\"cfg-field\"><label>Включить постоянный Wi-Fi <span id=\"def_webserverenable\" class=\"badge-default\"></span></label><select id=\"c_webserverenable\"><option value=\"true\">true</option><option value=\"false\">false</option></select></div>"
"<div class=\"cfg-field\"><label>Web User (Read-Only) <span id=\"def_webuser\" class=\"badge-default\"></span></label><input type=\"text\" id=\"c_webuser\" disabled maxlength=\"31\"></div>"
"<div class=\"cfg-field\"><label>Web Пароль (max 63) <span id=\"def_webpassword\" class=\"badge-default\"></span></label><input type=\"password\" id=\"c_webpassword\" maxlength=\"63\" placeholder=\"••••••••\" oninput=\"cp('webpassword')\"></div>"
"<div class=\"cfg-field\" id=\"w_webpassword_c\" style=\"display:none;\"><label style=\"color:var(--warn);\">Повторите Web пароль</label><input type=\"password\" id=\"c_webpassword_c\" maxlength=\"63\" placeholder=\"Повторите пароль\"></div>"
"</div>"
"<div style=\"margin-top:20px;display:flex;gap:10px;align-items:center;\">"
"<button class=\"btn\" onclick=\"scf()\">💾 Сохранить в файл</button>"
"<button class=\"btn\" style=\"background:var(--border);color:#fff;\" onclick=\"lc()\">❌ Отмена</button>"
"</div>"
"<div id=\"c_st\" style=\"margin-top:12px;font-weight:bold;\"></div>"
"</div></div>"
"<div id=\"ota\" class=\"tab\"><div class=\"card\">"
"<h3 style=\"margin-bottom:10px;color:var(--primary);\">📋 Статус последнего OTA обновления</h3>"
"<div class=\"grid\">"
"<div class=\"stat\"><div class=\"stat-label\">Источник OTA</div><div class=\"stat-val\" id=\"ota_src\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Загрузка прошивки</div><div class=\"stat-val\" id=\"ota_dl\">-</div></div>"
"<div class=\"stat\"><div class=\"stat-label\">Первый запуск после OTA</div><div class=\"stat-val\" id=\"ota_bt\">-</div></div>"
"</div></div>"
"<div class=\"card\">"
"<h3 style=\"margin-bottom:10px;color:var(--primary);\">Загрузка прошивки</h3>"
"<div class=\"dropzone\" id=\"dz\" onclick=\"document.getElementById('fi').click()\">"
"<p>Выберите или перетащите файл прошивки <b>(*.bin)</b></p>"
"<span id=\"fn\" style=\"color:var(--sub);font-size:0.85rem;\">Файл не выбран</span>"
"<input type=\"file\" id=\"fi\" accept=\".bin\" style=\"display:none\" onchange=\"sf(this.files)\">"
"</div>"
"<button class=\"btn\" id=\"ub\" disabled onclick=\"up()\">Прошить</button>"
"<div class=\"pbar\" id=\"pb\"><div class=\"pfill\" id=\"pf\"></div></div>"
"<div id=\"st\" style=\"margin-top:10px;font-weight:bold;\"></div>"
"</div></div>"
"<div id=\"logs\" class=\"tab\"><div class=\"card\">"
"<div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;\">"
"<h3 style=\"color:var(--primary);\">Журнал работы</h3>"
"<div>"
"<label><input type=\"checkbox\" id=\"sc\" checked> Автопрокрутка</label>"
"<button class=\"btn\" style=\"padding:4px 8px;font-size:0.75rem;margin-left:8px;\" onclick=\"ll()\">Обновить</button>"
"<button class=\"btn\" style=\"padding:4px 8px;font-size:0.75rem;margin-left:4px;background:var(--border);color:#fff;\" onclick=\"cl()\">Очистить</button>"
"</div></div>"
"<div class=\"logbox\" id=\"lb\">Загрузка...</div>"
"</div></div>"
"<script>"
"let file=null,rc=null,rd=null;"
"function t(id,btn){document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.nav button').forEach(x=>x.classList.remove('active'));document.getElementById(id).classList.add('active');btn.classList.add('active');if(id==='logs')ll();if(id==='cfg')lc();}"
"function st(){fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('chk_nvs').innerHTML=d.nvs_ok?'<span class=\"badge-ok\">🟢 ОК</span>':'<span class=\"badge-err\">🔴 Ошибка</span>';"
"document.getElementById('chk_wifi').innerHTML=d.wifi_ok?'<span class=\"badge-ok\">🟢 ОК</span>':'<span class=\"badge-err\">🟡 Ошибка</span>';"
"document.getElementById('chk_sd').innerHTML=d.sd_ok?'<span class=\"badge-ok\">🟢 ОК</span>':'<span class=\"badge-err\">🔴 Ошибка</span>';"
"document.getElementById('chk_gnss').innerHTML=d.gnss_ok?'<span class=\"badge-ok\">🟢 ОК</span>':'<span class=\"badge-err\">🔴 Ошибка</span>';"
"document.getElementById('warn_banner').style.display=(!d.sd_ok||!d.gnss_ok)?'block':'none';"
"document.getElementById('wardriving_banner').style.display='block';"
"document.getElementById('bld').innerText=d.build_date||'-';"
"document.getElementById('fwv').innerText=d.fw_version||'-';"
"document.getElementById('hp').innerText=(d.heap/1024).toFixed(1)+' KB';"
"let u=d.uptime; let h=Math.floor(u/3600); let m=Math.floor((u%3600)/60); let s=Math.floor(u%60);"
"let hs=(h>0)?(h+':'):''; let ms=(m<10&&h>0?'0'+m:m)+':'; let ss=(s<10?'0'+s:s);"
"document.getElementById('ut').innerText=hs+ms+ss;"
"document.getElementById('rr').innerText=d.reset_reason||'-';"
"document.getElementById('fix').innerText=d.fix?'3D FIX':'No Fix';"
"document.getElementById('pos').innerText=d.fix?(d.lat.toFixed(5)+', '+d.lon.toFixed(5)):'-';"
"document.getElementById('sat').innerText=d.sats||'0';"
"if(d.ota_status){let os=d.ota_status;"
"document.getElementById('ota_src').innerText=os.source||'Нет';"
"document.getElementById('ota_dl').innerHTML=os.download_ok?'<span class=\"badge-ok\">🟢 Успешно</span>':(os.source!=='Нет'?'<span class=\"badge-err\">🔴 Ошибка ('+os.error_msg+')</span>':'-');"
"let bt_str='-';"
"if(os.pending_verify){bt_str='<span style=\"color:var(--warn);font-weight:bold;\">🟡 Ожидание проверки</span>';}"
"else if(os.first_boot_ok){bt_str='<span class=\"badge-ok\">🟢 Успешно</span>';}"
"else if(os.source!=='Нет'&&os.download_ok){bt_str='<span class=\"badge-err\">🔴 Сбой (Откат Rollback)</span>';}"
"document.getElementById('ota_bt').innerHTML=bt_str;}"
"}).catch(e=>console.error(e));}"
"setInterval(st,3000);st();"
"function rb(){if(confirm('Перезагрузить ESP32?')){fetch('/api/reboot',{method:'POST'}).then(()=>{alert('ESP32 перезагружается...');setTimeout(()=>location.reload(),5000);});}}"
"function sf(fl){if(fl.length>0){let f=fl[0];if(!f.name.toLowerCase().endsWith('.bin')){alert('Пожалуйста, выберите файл с расширением .bin!');return;}file=f;document.getElementById('fn').innerText=file.name+' ('+(file.size/1024).toFixed(1)+' KB)';document.getElementById('ub').disabled=false;}}"
"const dz=document.getElementById('dz');dz.ondragover=dz.ondragenter=(e)=>{e.preventDefault();};dz.ondrop=(e)=>{e.preventDefault();sf(e.dataTransfer.files);};"
"function up(){if(!file)return;const btn=document.getElementById('ub'),bar=document.getElementById('pb'),fill=document.getElementById('pf'),st=document.getElementById('st');"
"btn.disabled=true;bar.style.display='block';st.style.color='var(--primary)';st.innerText='Передача...';"
"const x=new XMLHttpRequest();x.open('POST','/api/update',true);"
"x.upload.onprogress=(e)=>{if(e.lengthComputable){const p=(e.loaded/e.total*100).toFixed(0);fill.style.width=p+'%';st.innerText='Загрузка: '+p+'%';}};"
"x.onload=()=>{if(x.status===200){st.style.color='var(--ok)';st.innerText='✅ Прошивка проверена! Перезагрузка через 3 сек...';}else{st.style.color='var(--err)';st.innerText='❌ Ошибка: '+x.responseText;btn.disabled=false;}};"
"x.onerror=()=>{st.style.color='var(--err)';st.innerText='❌ Ошибка сети!';btn.disabled=false;};x.send(file);}"
"function ll(){fetch('/api/log').then(r=>r.text()).then(t=>{const box=document.getElementById('lb');box.innerText=t||'Лог пуст';if(document.getElementById('sc').checked)box.scrollTop=box.scrollHeight;});}"
"function cl(){fetch('/api/log?clear=1').then(()=>ll());}"
"function cp(id){const v=document.getElementById('c_'+id).value;document.getElementById('w_'+id+'_c').style.display=v.length>0?'flex':'none';}"
"function bd(k,v,d){const el=document.getElementById('def_'+k);if(el){el.innerText=(v===d)?'[По умолчанию]':'';}}"
"function lc(){fetch('/api/config').then(r=>r.json()).then(res=>{"
"rc=res.config;rd=res.defaults;const c=rc,d=rd;"
"document.getElementById('c_ssid').value=c.ssid||'';"
"document.getElementById('c_wifipasswd').value='';"
"document.getElementById('c_wifipasswd_c').value='';"
"document.getElementById('w_wifipasswd_c').style.display='none';"
"document.getElementById('c_ftpaddress').value=c.ftpaddress||'';"
"document.getElementById('c_ftpuser').value=c.ftpuser||'';"
"document.getElementById('c_ftppassword').value='';"
"document.getElementById('c_ftppassword_c').value='';"
"document.getElementById('w_ftppassword_c').style.display='none';"
"document.getElementById('c_ftpport').value=c.ftpport||21;"
"document.getElementById('c_filesize').value=c.filesize||1000000;"
"document.getElementById('c_gpssend').value=c.gpssend?'true':'false';"
"document.getElementById('c_wifisend').value=c.wifisend?'true':'false';"
"document.getElementById('c_ftpbeep').value=c.ftpbeep?'true':'false';"
"document.getElementById('c_moduleid').value=c.moduleid||'00';"
"document.getElementById('c_paccmask').value=c.paccmask||0;"
"document.getElementById('c_pdopmask').value=c.pdopmask||0;"
"document.getElementById('c_timezone').value=(c.timezone!==undefined)?c.timezone:5;"
"document.getElementById('c_mintracksize').value=c.min_track_size||1000;"
"document.getElementById('c_webserverenable').value=c.webserverenable?'true':'false';"
"document.getElementById('c_webuser').value=c.webuser||'';"
"document.getElementById('c_webpassword').value='';"
"document.getElementById('c_webpassword_c').value='';"
"document.getElementById('w_webpassword_c').style.display='none';"
"document.getElementById('c_st').innerText='';"
"bd('ssid',c.ssid,d.ssid);"
"bd('wifipasswd',c.wifipasswd,d.wifipasswd);"
"bd('ftpaddress',c.ftpaddress,d.ftpaddress);"
"bd('ftpuser',c.ftpuser,d.ftpuser);"
"bd('ftppassword',c.ftppassword,d.ftppassword);"
"bd('ftpport',c.ftpport,d.ftpport);"
"bd('filesize',c.filesize,d.filesize);"
"bd('gpssend',c.gpssend,d.gpssend);"
"bd('wifisend',c.wifisend,d.wifisend);"
"bd('ftpbeep',c.ftpbeep,d.ftpbeep);"
"bd('moduleid',c.moduleid,d.moduleid);"
"bd('paccmask',c.paccmask,d.paccmask);"
"bd('pdopmask',c.pdopmask,d.pdopmask);"
"bd('timezone',c.timezone,d.timezone);"
"bd('mintracksize',c.min_track_size,d.min_track_size);"
"bd('webserverenable',c.webserverenable,d.webserverenable);"
"bd('webuser',c.webuser,d.webuser);"
"bd('webpassword',c.webpassword,d.webpassword);"
"}).catch(e=>console.error(e));}"
"function scf(){if(!rc)return;const st=document.getElementById('c_st');"
"const wp=document.getElementById('c_wifipasswd').value,wpc=document.getElementById('c_wifipasswd_c').value;"
"if(wp.length>0&&wp!==wpc){st.style.color='var(--err)';st.innerText='❌ Пароли Wi-Fi не совпадают!';return;}"
"const fp=document.getElementById('c_ftppassword').value,fpc=document.getElementById('c_ftppassword_c').value;"
"if(fp.length>0&&fp!==fpc){st.style.color='var(--err)';st.innerText='❌ FTP пароли не совпадают!';return;}"
"const wbp=document.getElementById('c_webpassword').value,wbpc=document.getElementById('c_webpassword_c').value;"
"if(wbp.length>0&&wbp!==wbpc){st.style.color='var(--err)';st.innerText='❌ Web пароли не совпадают!';return;}"
"let body={"
"ssid:rc.ssid,"
"wifipasswd:wp.length>0?wp:rc.wifipasswd,"
"ftpaddress:rc.ftpaddress,"
"ftpuser:rc.ftpuser,"
"ftppassword:fp.length>0?fp:rc.ftppassword,"
"ftpport:parseInt(document.getElementById('c_ftpport').value||'21',10),"
"filesize:parseInt(document.getElementById('c_filesize').value||'1000000',10),"
"gpssend:document.getElementById('c_gpssend').value==='true',"
"wifisend:document.getElementById('c_wifisend').value==='true',"
"ftpbeep:document.getElementById('c_ftpbeep').value==='true',"
"moduleid:document.getElementById('c_moduleid').value||'00',"
"paccmask:parseInt(document.getElementById('c_paccmask').value||'0',10),"
"pdopmask:parseInt(document.getElementById('c_pdopmask').value||'0',10),"
"timezone:parseInt(document.getElementById('c_timezone').value||'5',10),"
"min_track_size:parseInt(document.getElementById('c_mintracksize').value||'1000',10),"
"webserverenable:document.getElementById('c_webserverenable').value==='true',"
"webuser:rc.webuser,"
"webpassword:wbp.length>0?wbp:rc.webpassword"
"};"
"st.style.color='var(--primary)';st.innerText='Сохранение...';"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(r=>{"
"if(r.status===200)return r.json();else throw new Error('Ошибка сервера '+r.status);"
"}).then(res=>{"
"st.style.color='var(--ok)';st.innerText='✅ '+res.message;"
"setTimeout(lc,1500);"
"}).catch(e=>{st.style.color='var(--err)';st.innerText='❌ '+e.message;});}"
"</script></body></html>";

// ====================================================================================
// Проверка авторизации HTTP Basic Auth
// ====================================================================================
static bool is_authenticated(httpd_req_t *req)
{
    if (strlen(g_app_config.webserver_user) == 0 || strlen(g_app_config.webserver_passwd) == 0) {
        return false;
    }

    char auth_hdr[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }

    char user_pass[128];
    snprintf(user_pass, sizeof(user_pass), "%s:%s", g_app_config.webserver_user, g_app_config.webserver_passwd);

    unsigned char expected_b64[256] = {0};
    size_t olen = 0;
    mbedtls_base64_encode(expected_b64, sizeof(expected_b64) - 1, &olen, (const unsigned char *)user_pass, strlen(user_pass));

    char expected_hdr[300];
    snprintf(expected_hdr, sizeof(expected_hdr), "Basic %s", (char *)expected_b64);

    return (strcmp(auth_hdr, expected_hdr) == 0);
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 GNSS Logger\"");
    httpd_resp_send(req, "401 Unauthorized. Access denied.", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ====================================================================================
// Handler GET / (Главная страница)
// ====================================================================================
static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ====================================================================================
// ====================================================================================
// Вспомогательная функция: Причина перезагрузки
// ====================================================================================
static const char* app_get_reset_reason_str(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON:   return "Включение питания";
        case ESP_RST_EXT:       return "Внешний сброс (Reset)";
        case ESP_RST_SW:        return "Программная перезагрузка";
        case ESP_RST_PANIC:     return "Аппаратное исключение (Паника)";
        case ESP_RST_INT_WDT:   return "Watchdog прерывания";
        case ESP_RST_TASK_WDT:  return "Watchdog задачи";
        case ESP_RST_WDT:       return "Watchdog таймер";
        case ESP_RST_DEEPSLEEP: return "Выход из Deep Sleep";
        case ESP_RST_BROWNOUT:  return "Просадка напряжения (Brownout)";
        case ESP_RST_SDIO:      return "Перезагрузка по SDIO";
        case ESP_RST_UNKNOWN:
        default:                return "Неизвестно";
    }
}

// ====================================================================================
// Handler GET /api/status (JSON статус системы)
// ====================================================================================
static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }

    ubx_nav_pvt_t pvt;
    /* D-5: проверяем не только наличие данных, но и валидность 3D-фикса */
    bool fix_ok = app_gnss_get_latest_pvt(&pvt) && app_gnss_is_fix_valid();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "build_date", BUILD_DATETIME);
    cJSON_AddStringToObject(root, "fw_version", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "heap", esp_get_free_internal_heap_size());
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddStringToObject(root, "reset_reason", app_get_reset_reason_str());
    cJSON_AddBoolToObject(root, "nvs_ok", g_system_checklist.nvs_config_ok);
    cJSON_AddBoolToObject(root, "wifi_ok", g_system_checklist.wifi_ok);
    cJSON_AddBoolToObject(root, "sd_ok", g_system_checklist.sd_card_ok);
    cJSON_AddBoolToObject(root, "gnss_ok", g_system_checklist.gnss_ok);
    cJSON_AddBoolToObject(root, "fix", fix_ok);

    if (fix_ok) {
        cJSON_AddNumberToObject(root, "lat", pvt.lat * 1e-7);
        cJSON_AddNumberToObject(root, "lon", pvt.lon * 1e-7);
        cJSON_AddNumberToObject(root, "sats", pvt.numSV);
    } else {
        cJSON_AddNumberToObject(root, "lat", 0);
        cJSON_AddNumberToObject(root, "lon", 0);
        cJSON_AddNumberToObject(root, "sats", 0);
    }

    app_ota_status_t ota_st;
    cJSON *ota_json = cJSON_CreateObject();
    if (app_ota_get_status(&ota_st) == ESP_OK) {
        if (ota_st.source == APP_OTA_SRC_SD) {
            cJSON_AddStringToObject(ota_json, "source", "SD-карта");
        } else if (ota_st.source == APP_OTA_SRC_WEB) {
            cJSON_AddStringToObject(ota_json, "source", "Веб-интерфейс");
        } else {
            cJSON_AddStringToObject(ota_json, "source", "Нет");
        }
        cJSON_AddBoolToObject(ota_json, "download_ok", ota_st.download_ok);
        cJSON_AddBoolToObject(ota_json, "first_boot_ok", ota_st.first_boot_ok);
        cJSON_AddBoolToObject(ota_json, "pending_verify", ota_st.pending_verify);
        cJSON_AddStringToObject(ota_json, "error_msg", ota_st.error_msg);
    } else {
        cJSON_AddStringToObject(ota_json, "source", "Нет");
        cJSON_AddBoolToObject(ota_json, "download_ok", false);
        cJSON_AddBoolToObject(ota_json, "first_boot_ok", false);
        cJSON_AddBoolToObject(ota_json, "pending_verify", false);
        cJSON_AddStringToObject(ota_json, "error_msg", "");
    }
    cJSON_AddItemToObject(root, "ota_status", ota_json);

    const char *resp = cJSON_PrintUnformatted(root);
    if (resp == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    free((void *)resp);
    cJSON_Delete(root);
    return ESP_OK;
}

/* O-3: единая функция перезапуска с настраиваемой задержкой через параметр.
 * delay_ms передаётся как uintptr_t через pvParameters. */
static void reboot_task(void *pvParameters)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)pvParameters;
    if (delay_ms == 0) delay_ms = 1000;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

// ====================================================================================
// Handler POST /api/reboot (Перезагрузка устройства)
// ====================================================================================
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }

    ESP_LOGI(TAG, "User requested system reboot via Web API");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}", HTTPD_RESP_USE_STRLEN);

    /* O-3: используем единую задачу с задержкой 1 сек (передаётся параметром) */
    xTaskCreate(reboot_task, "user_reboot", 2048, (void *)(uintptr_t)1000, 5, NULL);
    return ESP_OK;
}

// ====================================================================================
// Handler GET /api/log (Получение / Очистка системного лога)
// ====================================================================================
static esp_err_t log_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (strstr(query, "clear=1")) {
            app_log_buffer_clear();
            httpd_resp_send(req, "Log cleared", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    char *buf = (char *)malloc(LOG_BUFFER_SIZE);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t len = app_log_buffer_get(buf, LOG_BUFFER_SIZE);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, buf, len);

    free(buf);
    return ESP_OK;
}


// ====================================================================================
// Handler POST /api/update (Безопасное HTTP OTA обновление)
// ====================================================================================
static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }

    ESP_LOGI(TAG, "Starting HTTP OTA Firmware Update...");

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get OTA update partition");
        app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Раздел OTA не найден");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No OTA partition found");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    bool first_chunk = true;

    char buf[1024];
    int received = 0;
    size_t total_received = 0;

    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        if (first_chunk) {
            first_chunk = false;
            // Валидация заголовка образа (Magic Byte должен быть 0xE9 для ESP32 image)
            uint8_t magic = (uint8_t)buf[0];
            if (magic != 0xE9) {
                ESP_LOGE(TAG, "Invalid magic byte: 0x%02X (0xE9 expected)", magic);
                app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Неверный заголовок файла (0xE9)");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid image header (0xE9 required)");
                return ESP_FAIL;
            }

            esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Ошибка esp_ota_begin");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
                return ESP_FAIL;
            }
            ota_started = true;
        }

        esp_err_t err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_end(ota_handle);
            app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Сбой записи в Flash");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        total_received += received;
    }

    // Если прервалось до вычистки или возник сбой сокета
    if (received < 0) {
        ESP_LOGE(TAG, "HTTP receive error during OTA upload");
        if (ota_started) {
            esp_ota_end(ota_handle); // Не вызываем esp_ota_set_boot_partition!
        }
        app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Обрыв сетевого соединения");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Connection interrupted");
        return ESP_FAIL;
    }

    if (!ota_started || total_received == 0) {
        ESP_LOGE(TAG, "Empty file uploaded!");
        app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Пустой файл прошивки");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty image file");
        return ESP_FAIL;
    }

    // Проверка SHA-256 хэша и валидации образа в конце
    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end validation failed: %s", esp_err_to_name(err));
        app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Ошибка валидации SHA-256");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware SHA-256 validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        app_ota_save_status(APP_OTA_SRC_WEB, false, false, false, "Ошибка выбора загрузочного раздела");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }

    // Сохраняем успешный статус загрузки OTA
    app_ota_save_status(APP_OTA_SRC_WEB, true, false, true, "");

    ESP_LOGI(TAG, "HTTP OTA Firmware update successful! Scheduling reboot...");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

    /* O-3: используем единую задачу с задержкой 3 сек (передаётся параметром) */
    xTaskCreate(reboot_task, "reboot_task", 2048, (void *)(uintptr_t)3000, 5, NULL);
    return ESP_OK;
}

// ====================================================================================
// Handlers GET & POST /api/config (Редактирование конфигурации)
// ====================================================================================
static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }

    app_config_t def_cfg;
    app_config_get_defaults(&def_cfg);

    app_config_t file_cfg;
    app_config_get_defaults(&file_cfg); // Заполняем дефолтами на случай, если файла нет
    app_config_read_sd_json(&file_cfg); // Читаем актуальный файл с SD-карты

    cJSON *root = cJSON_CreateObject();
    cJSON *cfg_obj = cJSON_CreateObject();
    cJSON *def_obj = cJSON_CreateObject();

    if (root == NULL || cfg_obj == NULL || def_obj == NULL) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Текущая конфигурация берется из файла (если он есть), чтобы редактор показывал то, что на диске
    cJSON_AddStringToObject(cfg_obj, "ssid", file_cfg.wifi_ssid);
    cJSON_AddStringToObject(cfg_obj, "wifipasswd", file_cfg.wifi_passwd);
    cJSON_AddStringToObject(cfg_obj, "ftpaddress", file_cfg.ftp_address);
    cJSON_AddStringToObject(cfg_obj, "ftpuser", file_cfg.ftp_user);
    cJSON_AddStringToObject(cfg_obj, "ftppassword", file_cfg.ftp_passwd);
    cJSON_AddNumberToObject(cfg_obj, "ftpport", file_cfg.ftp_port);
    cJSON_AddNumberToObject(cfg_obj, "filesize", file_cfg.filesize_limit);
    cJSON_AddBoolToObject(cfg_obj, "gpssend", file_cfg.gps_send);
    cJSON_AddBoolToObject(cfg_obj, "wifisend", file_cfg.wifi_send);
    cJSON_AddBoolToObject(cfg_obj, "ftpbeep", file_cfg.ftp_beep);
    cJSON_AddStringToObject(cfg_obj, "moduleid", file_cfg.module_id);
    cJSON_AddNumberToObject(cfg_obj, "paccmask", file_cfg.pacc_mask);
    cJSON_AddNumberToObject(cfg_obj, "pdopmask", file_cfg.pdop_mask);
    cJSON_AddNumberToObject(cfg_obj, "timezone", file_cfg.timezone_offset);
    cJSON_AddNumberToObject(cfg_obj, "min_track_size", file_cfg.min_track_size);
    cJSON_AddBoolToObject(cfg_obj, "webserverenable", file_cfg.webserver_enable);
    cJSON_AddStringToObject(cfg_obj, "webuser", file_cfg.webserver_user);
    cJSON_AddStringToObject(cfg_obj, "webpassword", file_cfg.webserver_passwd);

    // Параметры по умолчанию
    cJSON_AddStringToObject(def_obj, "ssid", def_cfg.wifi_ssid);
    cJSON_AddStringToObject(def_obj, "wifipasswd", def_cfg.wifi_passwd);
    cJSON_AddStringToObject(def_obj, "ftpaddress", def_cfg.ftp_address);
    cJSON_AddStringToObject(def_obj, "ftpuser", def_cfg.ftp_user);
    cJSON_AddStringToObject(def_obj, "ftppassword", def_cfg.ftp_passwd);
    cJSON_AddNumberToObject(def_obj, "ftpport", def_cfg.ftp_port);
    cJSON_AddNumberToObject(def_obj, "filesize", def_cfg.filesize_limit);
    cJSON_AddBoolToObject(def_obj, "gpssend", def_cfg.gps_send);
    cJSON_AddBoolToObject(def_obj, "wifisend", def_cfg.wifi_send);
    cJSON_AddBoolToObject(def_obj, "ftpbeep", def_cfg.ftp_beep);
    cJSON_AddStringToObject(def_obj, "moduleid", def_cfg.module_id);
    cJSON_AddNumberToObject(def_obj, "paccmask", def_cfg.pacc_mask);
    cJSON_AddNumberToObject(def_obj, "pdopmask", def_cfg.pdop_mask);
    cJSON_AddNumberToObject(def_obj, "timezone", def_cfg.timezone_offset);
    cJSON_AddNumberToObject(def_obj, "min_track_size", def_cfg.min_track_size);
    cJSON_AddBoolToObject(def_obj, "webserverenable", def_cfg.webserver_enable);
    cJSON_AddStringToObject(def_obj, "webuser", def_cfg.webserver_user);
    cJSON_AddStringToObject(def_obj, "webpassword", def_cfg.webserver_passwd);

    cJSON_AddItemToObject(root, "config", cfg_obj);
    cJSON_AddItemToObject(root, "defaults", def_obj);

    char *resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (resp == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, resp);
    cJSON_free(resp);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_401(req);
    }

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Недопустимый размер запроса");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Нехватка памяти");
        return ESP_FAIL;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Ошибка приема данных");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);

    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Неверный формат JSON");
        return ESP_FAIL;
    }

    // Защита Read-Only параметров: принудительно устанавливаем текущие значения из g_app_config
    cJSON_DeleteItemFromObject(json, "ssid");
    cJSON_AddStringToObject(json, "ssid", g_app_config.wifi_ssid);
    cJSON_DeleteItemFromObject(json, "ftpaddress");
    cJSON_AddStringToObject(json, "ftpaddress", g_app_config.ftp_address);
    cJSON_DeleteItemFromObject(json, "ftpuser");
    cJSON_AddStringToObject(json, "ftpuser", g_app_config.ftp_user);
    cJSON_DeleteItemFromObject(json, "webuser");
    cJSON_AddStringToObject(json, "webuser", g_app_config.webserver_user);

    char *clean_json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (clean_json_str == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, " Ошибка обработки JSON");
        return ESP_FAIL;
    }

    // Вызываем сохранение ТОЛЬКО в SD-файл (без применения в текущей RAM g_app_config)
    esp_err_t err = app_config_save_json_file_only(clean_json_str);
    cJSON_free(clean_json_str);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, " Ошибка сохранения в файл /sdcard/config.json");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Конфигурация успешно сохранена в файл /sdcard/config.json. Изменения применятся после перезагрузки.\"}");
    return ESP_OK;
}

bool app_webserver_is_enabled_in_config(void)
{
    return g_app_config.webserver_enable &&
           (strlen(g_app_config.webserver_user) > 0) &&
           (strlen(g_app_config.webserver_passwd) > 0);
}

// ====================================================================================
// Управление сервером
// ====================================================================================
esp_err_t app_webserver_start(void)
{
    if (s_server != NULL) {
        return ESP_OK; // Сервер уже запущен
    }

    // Проверка условий активации
    if (!app_webserver_is_enabled_in_config()) {
        ESP_LOGI(TAG, "Webserver activation conditions not met in config.");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 10;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting Webserver on port %d...", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP Webserver!");
        return ESP_FAIL;
    }

    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &root_uri);

    httpd_uri_t status_uri = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t config_get_uri = {
        .uri      = "/api/config",
        .method   = HTTP_GET,
        .handler  = config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &config_get_uri);

    httpd_uri_t config_post_uri = {
        .uri      = "/api/config",
        .method   = HTTP_POST,
        .handler  = config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &config_post_uri);

    httpd_uri_t log_uri = {
        .uri      = "/api/log",
        .method   = HTTP_GET,
        .handler  = log_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &log_uri);

    httpd_uri_t update_uri = {
        .uri      = "/api/update",
        .method   = HTTP_POST,
        .handler  = update_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &update_uri);

    httpd_uri_t reboot_uri = {
        .uri      = "/api/reboot",
        .method   = HTTP_POST,
        .handler  = reboot_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &reboot_uri);

    ESP_LOGI(TAG, "Webserver successfully started!");
    app_ota_mark_valid();
    return ESP_OK;
}

void app_webserver_stop(void)
{
    if (s_server != NULL) {
        ESP_LOGI(TAG, "Stopping Webserver...");
        httpd_stop(s_server);
        s_server = NULL;
    }
}

bool app_webserver_is_running(void)
{
    return (s_server != NULL);
}

static void webserver_monitor_task(void *pvParameters)
{
    for (;;) {
        /* Ожидаем подключения к WiFi */
        EventBits_t bits = xEventGroupWaitBits(g_network_event_group, BIT_WIFI_CONNECTED,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        
        if (bits & BIT_WIFI_CONNECTED) {
            /* Подключились. Проверяем конфиг и запускаем сервер, если нужно */
            g_system_checklist.wifi_ok = true;
            if (app_webserver_is_enabled_in_config() && !app_webserver_is_running()) {
                ESP_LOGI(TAG, "WiFi connected. Starting Webserver...");
                app_webserver_start();
            }
            
            /* Ждем, пока подключение активно */
            while (xEventGroupGetBits(g_network_event_group) & BIT_WIFI_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            
            /* Подключение разорвано. Останавливаем сервер */
            g_system_checklist.wifi_ok = false;
            if (app_webserver_is_running()) {
                ESP_LOGI(TAG, "WiFi disconnected. Stopping Webserver...");
                app_webserver_stop();
            }
        }
    }
}

void app_webserver_init(void)
{
    xTaskCreate(webserver_monitor_task, "webserver_monitor", 4096, NULL, 4, NULL);
}
