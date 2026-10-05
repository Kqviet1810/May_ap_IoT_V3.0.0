"""Run the production Wi-Fi publisher against timestamped driver flaps."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
src=(root/'MAYAP_INDUSTRIAL_v1_0_0/network_service.h').read_text()
def function(sig):
    start=src.index(sig); brace=src.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(src[end]=='{')-(src[end]=='}'); end+=1
    return src[start:end]
with tempfile.TemporaryDirectory() as name:
    out=pathlib.Path(name)
    globals=src[src.index('static volatile uint8_t publishedState'):src.index('static bool radioActive')]
    policy=root/'MAYAP_INDUSTRIAL_v1_0_0/wifi_stable_state.h'
    if policy.exists(): globals=policy.read_text()+'\n'+globals
    (out/'actual-wifi-globals.inc').write_text(globals)
    (out/'actual-wifi-publish.inc').write_text(function('inline void publish(')+'\n'+function('inline void applyWifiPowerMode('))
    (out/'actual-wifi-getters.inc').write_text(function('inline NetworkStatus mayapGetNetworkStatus(')+'\n'+function('inline NetworkStatus mayapGetRawNetworkStatus(')+'\n'+function('inline void tickStableWifi('))
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(out),str(root/'tests/runtime-wifi-state.cpp'),'-o',str(out/'test')],check=True)
    subprocess.run([str(out/'test')],check=True)
