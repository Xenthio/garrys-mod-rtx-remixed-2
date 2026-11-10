import assert from 'node:assert/strict';
import {rotationAxes, rotateWorld} from '../tools/probe_editor/rotation.mjs';

function close(a,b) {
  for(let i=0;i<3;i++)for(let j=0;j<3;j++)assert.ok(Math.abs(a[i][j]-b[i][j])<1e-6,`${a} != ${b}`);
}
for(const angles of [[0,0,0],[15,89,-120],[35,-55,95],[0,90,0],[10,-90,45]]) {
  for(let axis=0;axis<3;axis++)for(const angle of [-2.5,0,.8]) {
    const source=rotationAxes(angles), actual=rotationAxes(rotateWorld(angles,axis,angle));
    const c=Math.cos(angle),s=Math.sin(angle),a=(axis+1)%3,b=(axis+2)%3;
    const expected=source.map(p=>{const q=[...p];q[a]=c*p[a]-s*p[b];q[b]=s*p[a]+c*p[b];return q;});
    close(actual,expected);
  }
}
console.log('World-axis rotation composition: 45 cases passed');
