import {rotationAxes} from './rotation.mjs';
import {add,sub,mul,dot,basis,project,ray,axisDistance,planePoint,resizeFace,framePosition} from './perspective.mjs';
import {FlyControls} from './fly_controls.mjs';

// Solid, depth-tested map geometry lives below an independent authoring overlay.
export class Viewport {
  constructor(element,callbacks) {
    this.element=element; this.callbacks=callbacks;
    this.canvas=element.querySelector('.solid'); this.overlay=element.querySelector('.gizmos');
    this.camera={position:[0,0,128],yaw:0,pitch:0}; this.speed=300; this.handles=[];
    const gl=this.canvas.getContext('webgl',{antialias:true});
    if(!gl)throw Error('The 3D editor requires WebGL');
    this.gl=gl;
    const shader=(type,source)=>{
      const result=gl.createShader(type); gl.shaderSource(result,source); gl.compileShader(result);
      if(!gl.getShaderParameter(result,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(result));
      return result;
    };
    const program=gl.createProgram();
    gl.attachShader(program,shader(gl.VERTEX_SHADER,`
      attribute vec3 position; attribute vec3 normal;
      uniform vec3 eye,forward,right,up; uniform float aspect;
      varying float shade;
      void main(){
        vec3 d=position-eye; float z=dot(d,forward);
        gl_Position=vec4(dot(d,right)*1.7320508/aspect,dot(d,up)*1.7320508,
          1.00002*z-2.00002,z);
        shade=.3+.7*abs(dot(normal,normalize(vec3(.35,.5,.8))));
      }`));
    gl.attachShader(program,shader(gl.FRAGMENT_SHADER,`
      precision mediump float; varying float shade;
      void main(){gl_FragColor=vec4(vec3(.38,.43,.47)*shade,1.0);}`));
    gl.linkProgram(program);
    if(!gl.getProgramParameter(program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(program));
    this.program=program;this.buffer=gl.createBuffer();
    this.uniforms=Object.fromEntries(['eye','forward','right','up','aspect'].map(n=>[n,gl.getUniformLocation(program,n)]));
    gl.useProgram(program); gl.bindBuffer(gl.ARRAY_BUFFER,this.buffer);
    for(const [name,offset] of [['position',0],['normal',12]]) {
      const location=gl.getAttribLocation(program,name);gl.enableVertexAttribArray(location);
      gl.vertexAttribPointer(location,3,gl.FLOAT,false,24,offset);
    }
    gl.enable(gl.DEPTH_TEST);
    this.overlay.oncontextmenu=e=>e.preventDefault();
    this.overlay.onpointerdown=e=>this.down(e);
    this.overlay.onpointermove=e=>this.move(e);
    this.flight=new FlyControls(this.overlay,{window,document},()=>{
      if(this.drag)this.callbacks.finish();this.drag=null;
    });
    this.overlay.onpointerup=e=>{
      this.flight.stop();
      if(this.overlay.hasPointerCapture(e.pointerId))this.overlay.releasePointerCapture(e.pointerId);
    };
    this.overlay.onpointercancel=()=>this.flight.stop();
    this.overlay.onlostpointercapture=()=>this.flight.lostCapture();
    this.overlay.onwheel=e=>{e.preventDefault();this.speed=Math.max(10,Math.min(10000,this.speed*Math.exp(-e.deltaY*.002)));this.draw();};
    new ResizeObserver(()=>this.draw()).observe(element);
    let previous;
    const tick=time=>{
      const dt=Math.min(.05,(time-(previous??time))/1000);previous=time;
      if(this.flight.active&&this.flight.keys.size){
        const b=basis(this.camera.yaw,this.camera.pitch),k=code=>Number(this.flight.keys.has(code));
        const motion=add(add(mul(b.forward,k('KeyW')-k('KeyS')),mul(b.right,k('KeyD')-k('KeyA'))),[0,0,k('KeyE')-k('KeyQ')]);
        const length=Math.hypot(...motion),boost=k('ShiftLeft')||k('ShiftRight')?3:1;
        if(length)this.camera.position=add(this.camera.position,mul(motion,dt*this.speed*boost/length));
        this.draw();
      }
      requestAnimationFrame(tick);
    };requestAnimationFrame(tick);
  }
  setScene(world) {
    const gl=this.gl;gl.bindBuffer(gl.ARRAY_BUFFER,this.buffer);
    gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(world.mesh),gl.STATIC_DRAW);this.count=world.mesh.length/6;
  }
  frame(points) {
    if(!points.length)return;
    this.camera.yaw=Math.PI/4;this.camera.pitch=-.3;
    this.camera.position=framePosition(points,this.camera,this.overlay.clientWidth,this.overlay.clientHeight);this.draw();
  }
  goToCapture(volume) {
    this.camera.position=[...volume.capture];
    this.draw();
  }
  position(e){const r=this.overlay.getBoundingClientRect();return[e.clientX-r.left,e.clientY-r.top];}
  mouseRay(mouse){return ray(mouse,this.camera,this.overlay.clientWidth,this.overlay.clientHeight);}
  down(e) {
    if(e.button===2){
      e.preventDefault();this.flight.start(e.pointerId);return;
    }
    if(e.button!==0)return;
    const mouse=this.position(e),hits=this.handles.filter(h=>Math.hypot(mouse[0]-h.p[0],mouse[1]-h.p[1])<12);
    const hit=(e.altKey?hits.find(h=>h.kind==='move'):null)||hits.find(h=>h.kind==='face')||hits.at(-1);
    if(!hit)return;
    if(hit.kind==='select'){this.callbacks.select(hit.index);return;}
    const volume=structuredClone(this.callbacks.state().data.volumes[hit.index]);
    const normal=basis(this.camera.yaw,this.camera.pitch).forward;
    const origin=hit.kind==='capture'?volume.capture:volume.center;
    const axis=hit.kind==='face'?rotationAxes(volume.rotationDegrees)[hit.axis]:null;
    const start=axis?axisDistance(this.mouseRay(mouse),origin,axis):planePoint(this.mouseRay(mouse),origin,normal);
    if(start===null)return;
    this.callbacks.checkpoint();this.drag={...hit,volume,normal,origin,axisVector:axis,start};
    this.overlay.setPointerCapture(e.pointerId);
  }
  move(e) {
    if(this.flight.active){this.camera.yaw-=e.movementX*.003;this.camera.pitch=Math.max(-1.55,Math.min(1.55,this.camera.pitch-e.movementY*.003));this.draw();return;}
    if(!this.drag)return;
    const d=this.drag,volume=this.callbacks.state().data.volumes[d.index],r=this.mouseRay(this.position(e));
    if(d.kind==='face'){
      const distance=axisDistance(r,d.origin,d.axisVector);if(distance===null)return;
      Object.assign(volume,resizeFace(d.volume,d.axis,d.sign,distance-d.start,rotationAxes(d.volume.rotationDegrees)));
    }else{
      const point=planePoint(r,d.origin,d.normal);if(!point)return;
      const key=d.kind==='capture'?'capture':'center';volume[key]=add(d.volume[key],sub(point,d.start));
    }
    this.callbacks.changed();
  }
  draw() {
    const state=this.callbacks.state();if(!state.world||!state.data)return;
    const width=this.overlay.clientWidth,height=this.overlay.clientHeight;if(!width||!height)return;
    const ratio=devicePixelRatio||1;
    for(const c of [this.canvas,this.overlay])if(c.width!==Math.round(width*ratio)||c.height!==Math.round(height*ratio)){c.width=Math.round(width*ratio);c.height=Math.round(height*ratio);}
    const gl=this.gl,b=basis(this.camera.yaw,this.camera.pitch);gl.viewport(0,0,this.canvas.width,this.canvas.height);
    gl.clearColor(.07,.1,.13,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);gl.useProgram(this.program);
    gl.uniform3fv(this.uniforms.eye,this.camera.position);
    for(const key of ['forward','right','up'])gl.uniform3fv(this.uniforms[key],b[key]);
    gl.uniform1f(this.uniforms.aspect,width/height);gl.drawArrays(gl.TRIANGLES,0,this.count||0);
    const ctx=this.overlay.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,width,height);
    const screen=p=>project(p,this.camera,width,height);
    const mark=(p,color,size,square=false)=>{ctx.fillStyle=color;ctx.beginPath();if(square)ctx.rect(p[0]-size,p[1]-size,2*size,2*size);else ctx.arc(p[0],p[1],size,0,Math.PI*2);ctx.fill();};
    this.handles=[];
    const volumeDraw=(v,index,reference=false)=>{
      const axes=rotationAxes(v.rotationDegrees),chosen=index===state.selected&&!reference;
      const corners=Array.from({length:8},(_,i)=>axes.reduce((p,a,j)=>add(p,mul(a,v.halfSize[j]*(i&(1<<j)?1:-1))),v.center));
      ctx.strokeStyle=reference?'#d68b6c':chosen?'#7de1ce':'#688c9b';ctx.lineWidth=chosen?2:1;ctx.setLineDash(reference?[4,5]:[]);
      for(let i=0;i<8;i++)for(let j=0;j<3;j++)if(!(i&(1<<j))){
        let a=corners[i],c=corners[i|(1<<j)],za=dot(sub(a,this.camera.position),b.forward),zc=dot(sub(c,this.camera.position),b.forward);
        if(za<=1&&zc<=1)continue;
        if(za<=1)a=add(a,mul(sub(c,a),(1.01-za)/(zc-za)));
        if(zc<=1)c=add(c,mul(sub(a,c),(1.01-zc)/(dot(sub(a,this.camera.position),b.forward)-zc)));
        const p=screen(a),q=screen(c);if(!p||!q)continue;ctx.beginPath();ctx.moveTo(p[0],p[1]);ctx.lineTo(q[0],q[1]);ctx.stroke();
      }
      ctx.setLineDash([]);
      const capture=screen(v.capture),center=screen(v.center);
      if(capture){mark(capture,'#8bc4ff',chosen?6:4);ctx.fillStyle='#def7ee';ctx.fillText(`${v.name} [${v.priority}]`,capture[0]+10,capture[1]-10);
        if(!reference)this.handles.push({kind:chosen?'capture':'select',index,p:capture});}
      if(!chosen)return;
      if(center){mark(center,'#7de1ce',5,true);this.handles.push({kind:'move',index,p:center});}
      for(let axis=0;axis<3;axis++)for(const sign of [-1,1]){
        const p=screen(add(v.center,mul(axes[axis],v.halfSize[axis]*sign)));
        if(p){mark(p,'#ffd18a',5,true);this.handles.push({kind:'face',index,axis,sign,p});}
      }
      // Capture wins an overlapping center hit; Alt chooses bounds movement.
      if(capture)this.handles.push({kind:'capture',index,p:capture});
    };
    if(state.reference)for(const v of state.world.referenceVolumes||[])volumeDraw(v,-1,true);
    state.data.volumes.forEach((v,i)=>volumeDraw(v,i));
    ctx.fillStyle='#dbe5ee';ctx.fillText(`RMB + WASD fly · Q/E down/up · Shift faster · wheel speed: ${Math.round(this.speed)} units/s`,14,height-16);
  }
}
