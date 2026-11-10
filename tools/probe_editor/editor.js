import {rotationAxes, rotateWorld} from './rotation.mjs';
import {Viewport} from './viewport.mjs';
const $ = id => document.getElementById(id);
const token = location.hash.slice(1);
const headers = {'X-Editor-Token': token, 'Content-Type': 'application/json'};
const request = async (path, options = {}) => {
  const response = await fetch(path, {headers, ...options});
  const result = await response.json();
  if (!response.ok) throw Error(result.error || response.statusText);
  return result;
};
const copy = value => structuredClone(value);
let world, data, selected = -1, dirty = false, history = [], future = [];
let saveTimer, saving = false, saveRequested = false, editVersion = 0;
const views = [];
const perspective = new Viewport($('perspective'), {
  state:()=>({world,data,selected,reference:$('showReference').checked}),
  select,checkpoint,changed,finish:properties
});
const add = (a,b) => a.map((v,i) => v+b[i]);
const sub = (a,b) => a.map((v,i) => v-b[i]);
const mul = (a,s) => a.map(v => v*s);
const dot = (a,b) => a.reduce((sum,v,i) => sum+v*b[i],0);
function axes(volume) {
  return rotationAxes(volume.rotationDegrees);
}
function corners(volume) {
  const basis=axes(volume);
  return Array.from({length:8},(_,index)=>basis.reduce((point,axis,i)=>
    add(point,mul(axis,volume.halfSize[i]*(index&(1<<i)?1:-1))),volume.center));
}
function message(text, error=false) { $('status').textContent=text; $('status').style.color=error?'#ff9b8e':'#88d8c5'; }
function checkpoint() { history.push(copy(data)); if(history.length>100)history.shift(); future=[]; }
function scheduleSave() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => $('save').click(), 500);
}
function changed(rebuild=false) {
  dirty=true; editVersion++; message('Unsaved changes');
  if(rebuild)properties(); list(); draw();
  if($('live').checked) scheduleSave();
}
function list() {
  const control=$('volumes'); control.replaceChildren();
  data.volumes.forEach((volume,index)=>{
    const option=new Option(`${volume.name || 'Cubemap'} · priority ${volume.priority}`,index);
    control.add(option);
  });
  control.selectedIndex=selected;
  $('undo').disabled=!history.length; $('redo').disabled=!future.length;
}
function properties() {
  const panel=$('properties'); panel.replaceChildren();
  const volume=data.volumes[selected]; if(!volume)return;
  for(const [key,label] of [['name','Name'],['priority','Priority'],['center','Bounds center · XYZ'],
      ['halfSize','Half size · XYZ'],['rotationDegrees','Rotation degrees · XYZ'],['capture','Capture point · XYZ']]) {
    const group=document.createElement('label'); group.className='property';
    const title=document.createElement('span'); title.textContent=label; group.append(title);
    const values=Array.isArray(volume[key])?volume[key]:[volume[key]], fields=document.createElement('div');
    if(values.length===3)fields.className='triple';
    values.forEach((value,i)=>{
      const input=document.createElement('input'); input.type=key==='name'?'text':'number';
      input.value=value; input.step=key==='priority'?'1':'any'; input.setAttribute('aria-label',`${label} ${'XYZ'[i]}`);
      input.addEventListener('change',()=>{
        const next=key==='name'?input.value:Number(input.value);
        if(key!=='name'&&(!Number.isFinite(next)||Math.abs(next)>1e8||
           (key==='halfSize'&&next<=0)||(key==='priority'&&(!Number.isInteger(next)||next < -32768||next > 32767)))) {
          input.value=value; message('Invalid value',true); return;
        }
        checkpoint();
        if(values.length===3)volume[key][i]=next; else volume[key]=next;
        changed();
      });
      fields.append(input);
    }); group.append(fields); panel.append(group);
  }
}
function select(index) { selected=index; list(); properties(); draw(); }
function frame(points, includePerspective=true) {
  if(!points.length)return;
  if(includePerspective)perspective.frame(points);
  for(const view of views) {
    const [a,b]=view.axes;
    let low=[Infinity,Infinity], high=[-Infinity,-Infinity];
    for(const point of points)for(let i=0;i<2;i++){
      const value=point[[a,b][i]];low[i]=Math.min(low[i],value);high[i]=Math.max(high[i],value);
    }
    view.center=low.map((v,i)=>(v+high[i])/2);
    view.scale=Math.max(.0001,Math.min(view.canvas.clientWidth/Math.max(high[0]-low[0],128),
      view.canvas.clientHeight/Math.max(high[1]-low[1],128))*.78);
  }
  draw();
}
function screen(view,point) {
  const [a,b]=view.axes;
  return [view.canvas.clientWidth/2+(point[a]-view.center[0])*view.scale,
          view.canvas.clientHeight/2-(point[b]-view.center[1])*view.scale];
}
function line(ctx,a,b) {ctx.beginPath();ctx.moveTo(...a);ctx.lineTo(...b);ctx.stroke();}
function mark(ctx,point,radius,color,square=false) {
  ctx.fillStyle=color;ctx.beginPath();
  if(square)ctx.rect(point[0]-radius,point[1]-radius,radius*2,radius*2);
  else ctx.arc(...point,radius,0,Math.PI*2);
  ctx.fill();
}
function draw() {
  if(!world||!data)return;
  perspective.draw();
  for(const view of views) {
    const {canvas}=view, ctx=canvas.getContext('2d'), ratio=devicePixelRatio||1;
    const width=canvas.clientWidth,height=canvas.clientHeight;
    if(canvas.width!==Math.round(width*ratio)||canvas.height!==Math.round(height*ratio)){
      canvas.width=Math.round(width*ratio);canvas.height=Math.round(height*ratio);
    }
    ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,width,height);
    ctx.lineWidth=1;ctx.strokeStyle='#30434e';ctx.beginPath();
    for(const edge of world.lines) {
      const a=screen(view,edge.slice(0,3)),b=screen(view,edge.slice(3));
      if(Math.max(a[0],b[0])<0||Math.min(a[0],b[0])>width||Math.max(a[1],b[1])<0||Math.min(a[1],b[1])>height)continue;
      ctx.moveTo(...a);ctx.lineTo(...b);
    }ctx.stroke();
    if($('showReference').checked && world.referenceVolumes) {
      ctx.save();ctx.strokeStyle='#d68b6c';ctx.setLineDash([4,5]);
      for(const volume of world.referenceVolumes) {
        const points=corners(volume).map(p=>screen(view,p));
        for(let i=0;i<8;i++)for(let axis=0;axis<3;axis++)if(!(i&(1<<axis)))line(ctx,points[i],points[i|(1<<axis)]);
        mark(ctx,screen(view,volume.capture),3,'#d68b6c');
      }
      ctx.restore();
    }
    view.handles=[];
    data.volumes.forEach((volume,index)=>{
      const points=corners(volume).map(p=>screen(view,p)), chosen=index===selected;
      ctx.strokeStyle=chosen?'#7de1ce':'#5c7f8c';ctx.lineWidth=chosen?2:1;
      for(let i=0;i<8;i++)for(let j=0;j<3;j++)if(!(i&(1<<j)))line(ctx,points[i],points[i|(1<<j)]);
      const capture=screen(view,volume.capture);mark(ctx,capture,chosen?6:4,'#8bc4ff');
      ctx.fillStyle=chosen?'#def7ee':'#93acb8';ctx.fillText(`${volume.name} [${volume.priority}]`,capture[0]+9,capture[1]-9);
      view.handles.push({kind:'select',index,p:capture});
      if(!chosen)return;
      const center=screen(view,volume.center); mark(ctx,center,5,'#7de1ce',true);
      const basis=axes(volume);
      for(let axis=0;axis<3;axis++) {
        const projected=[basis[axis][view.axes[0]],basis[axis][view.axes[1]]];
        if(Math.hypot(...projected)<.15)continue;
        for(const sign of [-1,1]) {
          const p=screen(view,add(volume.center,mul(basis[axis],volume.halfSize[axis]*sign)));
          mark(ctx,p,4,'#ffd18a',true);view.handles.push({kind:'face',index,p,axis,sign});
        }
      }
      const radius=48;ctx.strokeStyle='#de9e55';ctx.lineWidth=1;ctx.beginPath();ctx.arc(...center,radius,0,Math.PI*2);ctx.stroke();
      view.handles.push({kind:'rotate',index,p:center,radius});
      view.handles.push({kind:'move',index,p:center},{kind:'capture',index,p:capture});
    });
    ctx.fillStyle='#8aa5b4';ctx.fillText(`${(100/view.scale).toFixed(1)} units / 100 px`,12,height-12);
  }
}
for(const canvas of document.querySelectorAll('canvas[data-axes]')) {
  const view={canvas,axes:canvas.dataset.axes.split(',').map(Number),center:[0,0],scale:1,handles:[]};views.push(view);
  const position=event=>{const rect=canvas.getBoundingClientRect();return[event.clientX-rect.left,event.clientY-rect.top];};
  let drag;
  canvas.oncontextmenu=event=>event.preventDefault();
  canvas.onpointerdown=event=>{
    const mouse=position(event);
    if(event.button!==0) {drag={kind:'pan',mouse,center:[...view.center]};canvas.setPointerCapture(event.pointerId);return;}
    const candidates=view.handles.filter(h=>h.kind==='rotate'?
      Math.abs(Math.hypot(...sub(mouse,h.p))-h.radius)<6:Math.hypot(...sub(mouse,h.p))<10);
    const hit=(event.altKey?candidates.find(h=>h.kind==='move'):null)||
      candidates.reverse().find(h=>h.kind!=='rotate')||candidates[0];
    if(!hit)return;
    if(hit.kind==='select'){select(hit.index);return;}
    checkpoint();drag={...hit,mouse,original:copy(data.volumes[hit.index])};canvas.setPointerCapture(event.pointerId);
  };
  canvas.onpointermove=event=>{
    if(!drag)return;
    const mouse=position(event), delta=[(mouse[0]-drag.mouse[0])/view.scale,-(mouse[1]-drag.mouse[1])/view.scale];
    if(drag.kind==='pan'){view.center=drag.center.map((v,i)=>v-delta[i]);draw();return;}
    const volume=data.volumes[drag.index], original=drag.original, shift=[0,0,0];
    shift[view.axes[0]]=delta[0];shift[view.axes[1]]=delta[1];
    if(drag.kind==='move')volume.center=add(original.center,shift);
    if(drag.kind==='capture')volume.capture=add(original.capture,shift);
    if(drag.kind==='face') {
      const axis=axes(original)[drag.axis], projected=axis[view.axes[0]]**2+axis[view.axes[1]]**2;
      const distance=dot(shift,axis)/projected;
      const next=Math.max(.5,original.halfSize[drag.axis]+drag.sign*distance/2);
      volume.halfSize[drag.axis]=next;
      volume.center=add(original.center,mul(axis,drag.sign*(next-original.halfSize[drag.axis])));
    }
    if(drag.kind==='rotate') {
      const center=screen(view,original.center);
      const angle=Math.atan2(-(mouse[1]-center[1]),mouse[0]-center[0])-
        Math.atan2(-(drag.mouse[1]-center[1]),drag.mouse[0]-center[0]);
      // Screen XY/XZ/YZ has a right-handed normal +Z/-Y/+X.
      volume.rotationDegrees=rotateWorld(original.rotationDegrees,view.axes[2],
        angle*(view.axes[2]===1?-1:1));
    }
    changed();
  };
  const finish=()=>{if(drag&&drag.kind!=='pan')properties();drag=null;};
  canvas.onpointerup=finish;canvas.onpointercancel=finish;canvas.onlostpointercapture=finish;
  canvas.onwheel=event=>{
    event.preventDefault();const mouse=position(event),before=[(mouse[0]-canvas.clientWidth/2)/view.scale,-(mouse[1]-canvas.clientHeight/2)/view.scale];
    view.scale=Math.min(100,Math.max(.0001,view.scale*Math.exp(-event.deltaY*.001)));
    const after=[(mouse[0]-canvas.clientWidth/2)/view.scale,-(mouse[1]-canvas.clientHeight/2)/view.scale];
    view.center=add(view.center,sub(before,after));draw();
  };
  new ResizeObserver(draw).observe(canvas);
}
$('volumes').onchange=()=>select($('volumes').selectedIndex);
$('showReference').onchange=draw;
$('precision').ontoggle=()=>{
  if(!$('precision').open||!world)return;
  const volume=data.volumes[selected];
  frame(volume?[...corners(volume),volume.capture]:world.lines.flatMap(edge=>[edge.slice(0,3),edge.slice(3)]),false);
};
$('add').onclick=()=>{
  checkpoint();const center=[...perspective.camera.position];
  data.volumes.push({id:crypto.randomUUID(),name:`Cubemap ${data.volumes.length+1}`,priority:0,
    center:[...center],halfSize:[128,128,128],rotationDegrees:[0,0,0],capture:[...center]});
  selected=data.volumes.length-1;changed(true);
};
$('remove').onclick=()=>{if(selected<0)return;checkpoint();data.volumes.splice(selected,1);selected=Math.min(selected,data.volumes.length-1);changed(true);};
$('undo').onclick=()=>{if(!history.length)return;future.push(copy(data));data=history.pop();selected=Math.min(selected,data.volumes.length-1);changed(true);};
$('redo').onclick=()=>{if(!future.length)return;history.push(copy(data));data=future.pop();selected=Math.min(selected,data.volumes.length-1);changed(true);};
$('frameMap').onclick=()=>frame(world.lines.flatMap(line=>[line.slice(0,3),line.slice(3)]));
$('focus').onclick=()=>{const volume=data.volumes[selected];if(volume)frame([...corners(volume),volume.capture]);};
$('goCapture').onclick=()=>{const volume=data.volumes[selected];if(volume)perspective.goToCapture(volume);};
$('save').onclick=async()=>{
  if(saving) { saveRequested=true; return; }
  saving=true;
  const version=editVersion, payload=JSON.stringify(data);
  try {
    const result=await request('/document',{method:'POST',body:payload});
    if(editVersion===version) { dirty=false; message(`Saved ${result.saved}`); }
  } catch(error) { message(error.message,true); }
  finally {
    saving=false;
    if(saveRequested || ($('live').checked && editVersion!==version)) {
      saveRequested=false; scheduleSave();
    }
  }
};
$('live').onchange=()=>{ if($('live').checked && dirty) scheduleSave(); else clearTimeout(saveTimer); };
$('export').onclick=()=>{
  const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)+'\n'],{type:'application/json'}));
  const link=document.createElement('a');link.href=url;link.download=`${world.name}.cubemaps.json`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
};
$('import').onchange=async event=>{
  try {
    const incoming=JSON.parse(await event.target.files[0].text());
    if(incoming.version!==1||incoming.units!=='bsp'||incoming.mapSha256!==world.sha256||!Array.isArray(incoming.volumes))throw Error('Map fingerprint or document version does not match');
    for(const volume of incoming.volumes)for(const key of ['center','capture','halfSize','rotationDegrees'])
      if(!Array.isArray(volume[key])||volume[key].length!==3||!volume[key].every(Number.isFinite)||
        (key==='halfSize'&&volume[key].some(v=>v<=0)))throw Error(`Invalid ${key}`);
    checkpoint();data=incoming;selected=incoming.volumes.length?0:-1;changed(true);
  }catch(error){message(error.message,true);}finally{event.target.value='';}
};
window.onbeforeunload=event=>{if(dirty){event.preventDefault();event.returnValue='';}};
window.addEventListener('keydown',event=>{if((event.ctrlKey||event.metaKey)&&event.key.toLowerCase()==='s'){event.preventDefault();$('save').click();}});
try {
  [world,data]=await Promise.all([request('/scene'),request('/document')]);
  perspective.setScene(world);
  $('live').checked=!!world.runtimeLive;
  $('mapName').textContent=world.name;$('note').textContent=world.note;
  $('referenceControl').hidden=!world.referenceVolumes;
  select(data.volumes.length?0:-1);$('frameMap').click();message('Ready');
} catch(error) {message(error.message,true);}
