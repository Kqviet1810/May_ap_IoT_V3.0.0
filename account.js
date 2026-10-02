(() => {
  'use strict';
  const SESSION_KEY='mayap.account.session.v1';
  const base=String(window.MAYAP_WEB_CONFIG.cloudApiBase).replace(/\/+$/,'');
  const apiOrigin=new URL(base).origin;
  const nativeFetch=window.fetch.bind(window);
  const channel=typeof BroadcastChannel==='function' ? new BroadcastChannel('mayap-account') : null;
  let token='',account=null,pending=null,lastCheck=0,googleScript=null,loginPending=false;
  try {
    token=localStorage.getItem(SESSION_KEY) || sessionStorage.getItem(SESSION_KEY) || '';
    if(token)localStorage.setItem(SESSION_KEY,token);
    sessionStorage.removeItem(SESSION_KEY);
  } catch (_) {try {token=sessionStorage.getItem(SESSION_KEY) || '';} catch (_) {}}
  const api=async(path,init={})=>{
    const url=new URL(path instanceof Request ? path.url : String(path),base+'/');
    if(url.origin!==apiOrigin || !url.pathname.startsWith('/api/'))throw new Error('Invalid account API');
    const headers=new Headers(init.headers);
    if(token)headers.set('Authorization','Bearer '+token);
    const response=await nativeFetch(url.href,{...init,headers,credentials:'omit',cache:'no-store'});
    if(response.status===401 && account)expire();
    return response;
  };
  window.fetch=async(input,init)=>{
    const url=new URL(input instanceof Request ? input.url : String(input),location.href);
    if(url.origin!==apiOrigin || !url.pathname.startsWith('/api/'))return nativeFetch(input,init);
    const next={...init};
    if(typeof next.body==='string') {
      try {const data=JSON.parse(next.body);delete data.pairing_token;delete data.browser_sessions;next.body=JSON.stringify(data);} catch (_) {}
    }
    return api(url.href,next);
  };
  function setToken(value) {
    token=value;
    try {if(value)localStorage.setItem(SESSION_KEY,value);else localStorage.removeItem(SESSION_KEY);} catch (_) {}
    try {if(value)sessionStorage.setItem(SESSION_KEY,value);else sessionStorage.removeItem(SESSION_KEY);} catch (_) {}
  }
  function renderIdentity(user) {
    const name=user.name || user.email || 'Tài khoản Google';
    document.querySelectorAll('[data-account-name]').forEach(el=>{el.textContent=name;el.title=name;});
    const email=document.getElementById('accountEmail');if(email)email.textContent=user.email || '';
    let picture='';
    try {const url=new URL(user.picture);if(url.protocol==='https:' && /(^|\.)googleusercontent\.com$/.test(url.hostname))picture=url.href;} catch (_) {}
    document.querySelectorAll('[data-account-avatar]').forEach(root=>{
      root.replaceChildren();
      if(picture){const img=document.createElement('img');img.src=picture;img.alt='';img.referrerPolicy='no-referrer';
        img.onerror=()=>renderMissingPhoto(root);root.append(img);}
      else renderMissingPhoto(root);
    });
  }
  function renderMissingPhoto(root) {
    root.removeAttribute('aria-hidden');root.replaceChildren();
    const button=document.createElement('button');button.type='button';button.className='avatarRefresh';
    button.title='Cập nhật ảnh đại diện Google';button.setAttribute('aria-label',button.title);
    // A neutral missing-photo icon, never an invented Google portrait or initials.
    button.innerHTML='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" aria-hidden="true"><circle cx="12" cy="8" r="4"/><path d="M4 22v-2a8 8 0 0 1 16 0v2"/></svg>';
    button.onclick=()=>{
      const dialog=document.getElementById('profilePhotoDialog');
      if(!dialog.open)dialog.showModal();prepareGoogle();
    };root.append(button);
  }
  function expire() {
    if(account) {
      const prefix=`mayap.account.${account.user.sub}.`;
      try {for(let i=localStorage.length-1;i>=0;i--){const key=localStorage.key(i);if(key?.startsWith(prefix))localStorage.removeItem(key);}} catch (_) {}
    }
    setToken('');account=null;
    try {localStorage.removeItem('mayap.web.v10.mqtt.private');localStorage.removeItem('mayap.push.v1');} catch (_) {}
    window.dispatchEvent(new Event('mayap-logout'));
    // Keep the GitHub Pages project path, including installed PWAs.
    location.replace(location.pathname);
  }
  async function refresh() {
    if(pending)return pending;
    if(!token){document.documentElement.dataset.auth='guest';prepareGoogle();return null;}
    pending=(async()=>{
      try {
        const response=await api('/api/account/session');
        if(response.status===401){setToken('');document.documentElement.dataset.auth='guest';prepareGoogle();return null;}
        if(!response.ok)throw new Error('Session unavailable');
        const data=await response.json();
        if(!data.success || !data.user?.sub || !Array.isArray(data.devices))throw new Error('Invalid session');
        if(account && account.user.sub!==data.user.sub){expire();return null;}
        account=data;lastCheck=Date.now();
        renderIdentity(data.user);
        window.dispatchEvent(new CustomEvent('mayap-account-devices',{detail:data.devices}));return data;
      } catch (_) {
        document.getElementById('authMessage').textContent='Chưa kiểm tra được phiên đăng nhập. Hãy thử lại khi có mạng.';
        document.getElementById('authRetry')?.removeAttribute('hidden');return account;
      } finally {pending=null;}
    })();return pending;
  }
  function loadGoogle() {
    if(!googleScript)googleScript=new Promise((resolve,reject)=>{
      const script=document.createElement('script');script.src='https://accounts.google.com/gsi/client';script.async=true;
      script.onload=resolve;script.onerror=()=>{googleScript=null;reject(new Error('Google unavailable'));};document.head.append(script);
    });return googleScript;
  }
  let preparing=null,challengeAt=0;
  function showLoginError(message) {
    document.getElementById('loginError').textContent=message;
    const photoError=document.getElementById('profilePhotoError');if(photoError)photoError.textContent=message;
  }
  async function prepareGoogle() {
    if(preparing)return preparing;
    preparing=(async()=>{
      try {
        const [response]=await Promise.all([api('/api/account/google/challenge',{method:'POST'}),loadGoogle()]);
        const challenge=await response.json();
        if(!response.ok || !challenge.success)throw new Error('Login unavailable');
        challengeAt=Date.now();
        google.accounts.id.initialize({client_id:challenge.clientId,nonce:challenge.nonce,ux_mode:'popup',auto_select:false,
          callback:async result=>{
            if(loginPending)return;loginPending=true;
            try {
              const res=await api('/api/account/google/login',{method:'POST',headers:{'Content-Type':'application/json'},
                body:JSON.stringify({credential:result.credential,challenge:challenge.challenge})});
              const data=await res.json();
              if(!res.ok || !/^[a-f0-9]{64}$/.test(data.token || ''))throw new Error('Login rejected');
              setToken(data.token);location.replace(location.pathname);
            } catch (_) {showLoginError('Đăng nhập chưa hoàn tất. Vui lòng thử lại.');prepareGoogle();}
            finally {loginPending=false;}
          }});
        document.querySelectorAll('.googleLogin').forEach(root=>{
          root.replaceChildren();google.accounts.id.renderButton(root,{type:'standard',theme:'outline',size:'large',shape:'pill',
            text:'signin_with',locale:'vi',width:root.closest('#profilePhotoDialog')?Math.min(260,window.innerWidth-80):root.closest('.landingNav')?220:300});
        });showLoginError('');
      } catch (_) {
        showLoginError('Chưa tải được đăng nhập Google. Kiểm tra mạng rồi thử lại.');
        document.querySelectorAll('.googleLogin').forEach(root=>{
          root.replaceChildren();const button=document.createElement('button');button.textContent='Thử lại đăng nhập Google';
          button.onclick=prepareGoogle;root.append(button);
        });
      } finally {preparing=null;}
    })();return preparing;
  }
  async function logout() {
    try {
      const res=await api('/api/account/logout',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});
      if(!res.ok)throw new Error('Logout failed');channel?.postMessage('logout');expire();
    } catch (_) {document.getElementById('accountNotice').textContent='Chưa đăng xuất được. Kiểm tra kết nối rồi thử lại.';}
  }
  channel?.addEventListener('message',event=>{if(event.data==='logout')expire();});
  window.addEventListener('storage',event=>{
    if(event.key!==SESSION_KEY)return;
    if(!event.newValue){expire();return;}
    if(event.newValue!==token)location.replace(location.pathname);
  });
  window.MayapAccount={ready:null,refresh,api,get current(){return account;}};
  document.getElementById('logoutBtn')?.addEventListener('click',logout);
  document.getElementById('profilePhotoClose')?.addEventListener('click',()=>document.getElementById('profilePhotoDialog').close());
  document.getElementById('authRetry')?.addEventListener('click',async()=>{if(await refresh())location.reload();});
  setInterval(()=>{if(document.hidden)return;if(account && Date.now()-lastCheck>=300000)refresh();else if(!token && Date.now()-challengeAt>=240000)prepareGoogle();},60000);
  document.addEventListener('visibilitychange',()=>{
    if(document.hidden)return;
    if(account && Date.now()-lastCheck>=300000)refresh();else if(!token && Date.now()-challengeAt>=240000)prepareGoogle();
  });
  window.MayapAccount.ready=refresh();
})();
