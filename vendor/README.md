# Browser dependencies

`jsQR.min.js` is the existing QR decoder. Realtime uses the browser's native
WebSocket API and the first-party bounded client in `realtime_transport.js`.
MQTT.js and its bundle were removed after the WebSocket regression passed.
