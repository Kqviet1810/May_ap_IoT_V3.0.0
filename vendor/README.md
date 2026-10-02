# Browser dependencies

`mqtt.min.js` is the unmodified MQTT.js **5.13.2** browser bundle previously
loaded from jsDelivr. It now ships with the web app and is included in the
service-worker shell, avoiding an extra CDN connection on startup.

Source: https://cdn.jsdelivr.net/npm/mqtt@5.13.2/dist/mqtt.min.js

Upstream: https://github.com/mqttjs/MQTT.js/tree/v5.13.2

License: MIT, included in `mqtt-LICENSE.md`.

`jsQR.min.js` is the existing QR decoder; this change does not replace it.
