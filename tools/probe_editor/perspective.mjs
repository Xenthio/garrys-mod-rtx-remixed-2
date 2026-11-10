// BSP coordinates: Z up. Camera yaw zero looks along +X.
export const add = (a,b) => a.map((v,i)=>v+b[i]);
export const sub = (a,b) => a.map((v,i)=>v-b[i]);
export const mul = (a,s) => a.map(v=>v*s);
export const dot = (a,b) => a.reduce((s,v,i)=>s+v*b[i],0);
export function basis(yaw,pitch) {
  const c=Math.cos(yaw),s=Math.sin(yaw),p=Math.cos(pitch),q=Math.sin(pitch);
  return {forward:[c*p,s*p,q],right:[s,-c,0],up:[-c*q,-s*q,p]};
}
export function framePosition(points, camera, width, height) {
  if (!points.length) return camera.position;
  const low = [Infinity, Infinity, Infinity], high = [-Infinity, -Infinity, -Infinity];
  for (const point of points) for (let axis = 0; axis < 3; axis++) {
    low[axis] = Math.min(low[axis], point[axis]);
    high[axis] = Math.max(high[axis], point[axis]);
  }
  const center = mul(add(low, high), .5);
  const radius = Math.max(64, Math.hypot(...sub(high, low)) * .5);
  const verticalHalfAngle = Math.PI / 6;
  const aspect = Math.max(.001, width / Math.max(1, height));
  const horizontalHalfAngle = Math.atan(Math.tan(verticalHalfAngle) * aspect);
  // Fit a bounding sphere to the narrower field of view with a small margin.
  const distance = radius * 1.1 / Math.sin(Math.min(verticalHalfAngle, horizontalHalfAngle));
  return sub(center, mul(basis(camera.yaw, camera.pitch).forward, distance));
}
export function project(point,camera,width,height) {
  const b=basis(camera.yaw,camera.pitch),d=sub(point,camera.position),z=dot(d,b.forward);
  if(z<=1)return null;
  const focal=height/(2*Math.tan(Math.PI/6));
  return [width/2+dot(d,b.right)*focal/z,height/2-dot(d,b.up)*focal/z,z];
}
export function ray(mouse,camera,width,height) {
  const b=basis(camera.yaw,camera.pitch),focal=height/(2*Math.tan(Math.PI/6));
  const direction=add(b.forward,add(mul(b.right,(mouse[0]-width/2)/focal),mul(b.up,(height/2-mouse[1])/focal)));
  return {origin:camera.position,direction:mul(direction,1/Math.hypot(...direction))};
}
// Closest point on the gizmo axis to a camera ray. Parallel axes are not draggable.
export function axisDistance(ray,origin,axis) {
  const c=dot(ray.direction,axis),denominator=1-c*c;
  if(denominator<.002)return null;
  const offset=sub(ray.origin,origin);
  return (dot(offset,axis)-c*dot(offset,ray.direction))/denominator;
}
export function planePoint(ray,origin,normal) {
  const denominator=dot(ray.direction,normal);
  if(Math.abs(denominator)<.0001)return null;
  const distance=dot(sub(origin,ray.origin),normal)/denominator;
  return distance>0?add(ray.origin,mul(ray.direction,distance)):null;
}
export function resizeFace(volume,axisIndex,sign,distance,axes) {
  const halfSize=[...volume.halfSize];
  halfSize[axisIndex]=Math.max(.5,halfSize[axisIndex]+sign*distance/2);
  return {halfSize,center:add(volume.center,mul(axes[axisIndex],sign*(halfSize[axisIndex]-volume.halfSize[axisIndex])))};
}
