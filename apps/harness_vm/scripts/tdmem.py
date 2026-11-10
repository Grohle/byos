# Mide RSS de TDLib (x86_64, libtdjson prebuilt de python-telegram) hasta el estado WaitPhoneNumber.
import ctypes, json, time, os, sys, shutil
def rss():
    for l in open('/proc/self/status'):
        if l.startswith(('VmRSS','VmHWM')): print('  ', l.strip())
    return int([l for l in open('/proc/self/status') if l.startswith('VmRSS')][0].split()[1])
print('python base'); b=rss()
lib=ctypes.CDLL(sys.argv[1])
lib.td_create_client_id.restype=ctypes.c_int
lib.td_receive.restype=ctypes.c_char_p; lib.td_receive.argtypes=[ctypes.c_double]
lib.td_send.argtypes=[ctypes.c_int, ctypes.c_char_p]
lib.td_execute.restype=ctypes.c_char_p; lib.td_execute.argtypes=[ctypes.c_char_p]
lib.td_execute(json.dumps({'@type':'setLogVerbosityLevel','new_verbosity_level':1}).encode())
print('lib loaded'); a=rss()
d='/tmp/tdmem'; shutil.rmtree(d, ignore_errors=True)
c=lib.td_create_client_id()
lib.td_send(c, json.dumps({'@type':'getOption','name':'version'}).encode())
t0=time.time(); state=None
while time.time()-t0<60:
    r=lib.td_receive(1.0)
    if not r: continue
    ev=json.loads(r)
    if ev.get('@type')=='optionValueString' : print('version', ev.get('value'))
    if ev.get('@type')=='updateOption' and ev.get('name')=='version': print('version', ev['value'])
    if ev.get('@type')=='updateAuthorizationState':
        state=ev['authorization_state']['@type']; print('state', state, round(time.time()-t0,2),'s')
        if state=='authorizationStateWaitTdlibParameters':
            lib.td_send(c, json.dumps({'@type':'setTdlibParameters','use_test_dc':True,'database_directory':d,
              'use_message_database':True,'use_file_database':False,'use_chat_info_database':True,'use_secret_chats':False,
              'api_id':12345,'api_hash':'0123456789abcdef0123456789abcdef','system_language_code':'es',
              'device_model':'hiby-test','application_version':'0.1'}).encode())
        if state=='authorizationStateWaitPhoneNumber': break
time.sleep(5)
while lib.td_receive(0.1): pass
print('after init (WaitPhoneNumber, conectado a DC de pruebas)'); z=rss()
print('TDLIB_DELTA_kB lib_load=%d init=%d total=%d' % (a-b, z-a, z-b))
import subprocess; print('threads', len(os.listdir('/proc/self/task')))
