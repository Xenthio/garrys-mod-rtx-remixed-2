import assert from 'node:assert/strict';
import {rotationAxes} from '../tools/probe_editor/rotation.mjs';
import {add,sub,mul,dot,basis,project,ray,axisDistance,planePoint,resizeFace,framePosition} from '../tools/probe_editor/perspective.mjs';
const close=(a,b)=>assert.ok(Math.abs(a-b)<1e-7,`${a} != ${b}`);
let cases=0;
for(const yaw of [0,.7,-2])for(const pitch of [-1,0,1]){
  const camera={position:[21,-42,73],yaw,pitch},b=basis(yaw,pitch);
  for(const axis of Object.values(b))close(dot(axis,axis),1);
  close(dot(b.forward,b.right),0);close(dot(b.forward,b.up),0);close(dot(b.up,b.right),0);
  const p=add(camera.position,add(mul(b.forward,300),add(mul(b.right,40),mul(b.up,-25))));
  const pixel=project(p,camera,1200,800),r=ray(pixel,camera,1200,800);
  const actual=planePoint(r,p,b.forward);
  actual.forEach((v,i)=>close(v,p[i]));
  assert.equal(project(sub(camera.position,b.forward),camera,1200,800),null);
  cases++;
}
for(const rotation of [[0,0,0],[23,-44,71]])for(let axis=0;axis<3;axis++)for(const sign of [-1,1]){
  const axes=rotationAxes(rotation),v={center:[10,20,30],halfSize:[100,80,60]},original=structuredClone(v);
  for(const distance of [-500,-10,20,500]){
    const resized=resizeFace(v,axis,sign,distance,axes);
    const before=sub(v.center,mul(axes[axis],sign*v.halfSize[axis]));
    const after=sub(resized.center,mul(axes[axis],sign*resized.halfSize[axis]));
    before.forEach((value,i)=>close(value,after[i]));
    assert.ok(resized.halfSize[axis]>=.5);assert.deepEqual(v,original);cases++;
  }
}
const camera={position:[0,0,0],yaw:0,pitch:0};
for(const target of [[100,0,0],[100,30,0],[100,-50,0]]){
  const r=ray(project(target,camera,1200,800),camera,1200,800);
  close(axisDistance(r,[100,0,0],[0,1,0]),target[1]);cases++;
}
assert.equal(axisDistance({origin:[0,0,0],direction:[1,0,0]},[0,0,0],[1,0,0]),null);
const corners=Array.from({length:8},(_,i)=>[i&1?100:-100,i&2?300:-300,i&4?200:-200]);
for(const [width,height] of [[320,1000],[1600,600],[800,800]]) {
  const framed={yaw:.7,pitch:-.3};framed.position=framePosition(corners,framed,width,height);
  for(const point of corners){
    const pixel=project(point,framed,width,height);
    assert.ok(pixel&&pixel[0]>0&&pixel[0]<width&&pixel[1]>0&&pixel[1]<height);
    cases++;
  }
}
console.log(`${cases} perspective, ray picking and fixed opposite-face checks passed`);
