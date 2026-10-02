// GitHub Pages frontend + cross-origin Worker account API. No persistent browser/MQTT credentials.
(() => {
  'use strict';
  const key='mayap.web.v10.mqtt.private';
  const get=Storage.prototype.getItem, set=Storage.prototype.setItem, remove=Storage.prototype.removeItem;
  let mqtt=null;
  // Discard legacy secrets; never import a previously shared broker password.
  try { remove.call(localStorage,key); remove.call(localStorage,'mayap.web.v10.devices'); } catch (_) {}
  Storage.prototype.getItem=function(k){return this===localStorage && String(k)===key ? mqtt : get.call(this,k);};
  Storage.prototype.setItem=function(k,v){if(this===localStorage && String(k)===key)mqtt=String(v);else set.call(this,k,v);};
  Storage.prototype.removeItem=function(k){if(this===localStorage && String(k)===key)mqtt=null;else remove.call(this,k);};
  window.MAYAP_WEB_CONFIG=Object.freeze({mqttUrl:'',mqttUsername:'',mqttPassword:'',topicRoot:'mayap/v1',
    reconnectPeriodMs:2000,connectTimeoutMs:15000,keepaliveSeconds:30,sessionTtlMs:45000,sessionRefreshMs:3000,
    staleAfterMs:8000,offlineAfterMs:30000,brokerSilenceAfterMs:90000,commandTimeoutMs:10000,
    configTimeoutMs:15000,cloudApiBase:'https://mayap-push-worker.vietk-mayaptrung.workers.dev'});
})();
