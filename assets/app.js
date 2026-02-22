(function(){
  const root=document.documentElement;
  const form=document.getElementById('postForm');
  const msg=document.getElementById('message');
  const nick=document.getElementById('nickname');
  const statusEl=document.getElementById('status');
  const list=document.getElementById('messages');
  const chatScroll=document.getElementById('chatScroll');
  const cid=document.getElementById('client_id');
  const themeToggle=document.getElementById('themeToggle');

  function setTheme(mode){
    root.classList.toggle('dark',mode==='dark');
    root.setAttribute('data-theme',mode);
    if(themeToggle){
      themeToggle.textContent=mode==='dark'?'Day Mode':'Night Mode';
    }
  }

  const savedTheme=localStorage.getItem('tryhttpd_theme');
  const preferredDark=window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches;
  setTheme(savedTheme||(preferredDark?'dark':'light'));

  if(themeToggle){
    themeToggle.addEventListener('click',function(){
      const next=root.classList.contains('dark')?'light':'dark';
      localStorage.setItem('tryhttpd_theme',next);
      setTheme(next);
    });
  }

  let id=localStorage.getItem('tryhttpd_client_id');
  if(!id){
    id=(window.crypto&&crypto.randomUUID)
      ?crypto.randomUUID()
      :'client-'+Date.now()+'-'+Math.random().toString(16).slice(2);
    localStorage.setItem('tryhttpd_client_id',id);
  }
  cid.value=id;

  const savedNick=localStorage.getItem('tryhttpd_nickname');
  if(savedNick){nick.value=savedNick;}
  nick.addEventListener('change',()=>localStorage.setItem('tryhttpd_nickname',nick.value.trim()));

  msg.addEventListener('keydown',function(e){
    if(e.key==='Enter'&&!e.shiftKey){
      e.preventDefault();
      form.requestSubmit();
    }
  });

  function scrollMessagesToBottom(){
    chatScroll.scrollTop=chatScroll.scrollHeight;
  }

  function isNearBottom(){
    const threshold=40;
    return chatScroll.scrollHeight-chatScroll.scrollTop-chatScroll.clientHeight<threshold;
  }

  async function refreshMessages(){
    const keepPinned=isNearBottom();
    const res=await fetch('/messages',{headers:{'X-Requested-With':'fetch'}});
    if(!res.ok){throw new Error('Failed to fetch messages');}
    list.innerHTML=await res.text();
    if(keepPinned){scrollMessagesToBottom();}
  }

  form.addEventListener('submit', async function(e){
    e.preventDefault();
    statusEl.textContent='Posting...';
    const data=new URLSearchParams(new FormData(form));
    try{
      const res=await fetch('/post',{
        method:'POST',
        headers:{'Content-Type':'application/x-www-form-urlencoded','X-Requested-With':'fetch'},
        body:data.toString()
      });
      if(!res.ok){throw new Error('Post failed');}
      msg.value='';
      await refreshMessages();
      scrollMessagesToBottom();
      statusEl.textContent='Posted.';
    }catch(err){
      statusEl.textContent='Post failed. Try again.';
    }
  });

  if(typeof EventSource!=='undefined'){
    const events=new EventSource('/events');
    events.addEventListener('message', function(){
      refreshMessages().catch(()=>{});
    });
    events.onerror=function(){
      // Browser will auto-reconnect SSE. Keep quiet unless needed.
    };
  }else{
    setInterval(refreshMessages,5000);
  }

  scrollMessagesToBottom();
})();
