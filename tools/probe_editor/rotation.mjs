// Columns of Rz * Ry * Rx. Coordinates remain in the BSP's right-handed space.
export function rotationAxes(degrees) {
  const [x,y,z] = degrees.map(v=>v*Math.PI/180);
  const cx=Math.cos(x), sx=Math.sin(x), cy=Math.cos(y), sy=Math.sin(y), cz=Math.cos(z), sz=Math.sin(z);
  return [[cz*cy,sz*cy,-sy],[cz*sy*sx-sz*cx,sz*sy*sx+cz*cx,cy*sx],
          [cz*sy*cx+sz*sx,sz*sy*cx-cz*sx,cy*cx]];
}

export function rotateWorld(degrees, axis, radians) {
  const basis=rotationAxes(degrees), c=Math.cos(radians), s=Math.sin(radians);
  const next=basis.map(point=>{
    const p=[...point], a=(axis+1)%3, b=(axis+2)%3;
    p[a]=c*point[a]-s*point[b]; p[b]=s*point[a]+c*point[b]; return p;
  });
  const y=Math.asin(Math.max(-1,Math.min(1,-next[0][2])));
  const singular=Math.abs(Math.cos(y))<1e-7;
  const x=singular?Math.atan2(-next[2][1],next[1][1]):Math.atan2(next[1][2],next[2][2]);
  const z=singular?0:Math.atan2(next[0][1],next[0][0]);
  return [x,y,z].map(v=>v*180/Math.PI);
}
