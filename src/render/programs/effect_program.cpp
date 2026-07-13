#include "render/programs/effect_program.h"

#include <array>
#include <stdexcept>
#include <string>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
uniform mat3 u_transform;
varying vec2 v_uv;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local - u_rect_origin;
    v_uv = a_position;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kCommonFragment[] = R"(
precision highp float;

uniform vec2 u_rect_size;
uniform float u_time;
uniform float u_item_width;
uniform float u_item_height;
uniform vec4 u_bg_color;
uniform float u_radius;
uniform float u_alternative;
uniform float u_night;
uniform float u_cloud_amount;
uniform float u_intensity;
uniform vec3 u_sky_top;
uniform vec3 u_sky_bottom;
varying vec2 v_uv;
varying vec2 v_pixel;

// Literal condition-sky gradient (Option A): light near the top, deeper toward the bottom.
// NOTE: this is a binary day/night sky. A future enhancement (see weather_tab.cpp
// weatherEffectForCode) could interpolate a continuous dawn->dusk->night sky from the
// real sunrise/sunset times, matching Apple Weather more closely.
vec3 computeBase(vec2 uv) {
    return mix(u_sky_top, u_sky_bottom, smoothstep(0.0, 1.0, uv.y));
}

float roundedBoxSDF(vec2 center, vec2 size, float radius) {
    vec2 q = abs(center) - size + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float cornerMask() {
    vec2 pixelPos = v_uv * vec2(u_item_width, u_item_height);
    vec2 center = pixelPos - vec2(u_item_width, u_item_height) * 0.5;
    vec2 halfSize = vec2(u_item_width, u_item_height) * 0.5;
    float dist = roundedBoxSDF(center, halfSize, u_radius);
    return 1.0 - smoothstep(-1.0, 0.0, dist);
}
)";

  // --- Sky effect: sun (day) or stars (night) + optional cloud layer ---
  // Covers Clear (cloud 0), Mostly Sunny (~0.26), Partly Cloudy (~0.55).
  constexpr char kSkyFragment[] = R"(
float eh(vec2 p){p=fract(p*vec2(234.34,435.345));p+=dot(p,p+34.23);return fract(p.x*p.y);}
float en(vec2 p){vec2 i=floor(p);vec2 f=fract(p);f=f*f*(3.0-2.0*f);float a=eh(i),b=eh(i+vec2(1.,0.)),c=eh(i+vec2(0.,1.)),d=eh(i+vec2(1.,1.));return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);}
float sunRays(vec2 uv,vec2 s,float t){vec2 d=uv-s;float a=atan(d.y,d.x);float di=length(d);float r=sin(a*7.0+sin(t*0.25))*0.5+0.5;r=pow(r,3.0);float f=1.0-smoothstep(0.0,1.2,di);return r*f*0.15;}
float sunCore(vec2 uv,vec2 s,float t){vec2 d=uv-s;float di=length(d);float mf=exp(-di*15.0)*2.0;float fl=0.0;for(int i=1;i<=3;i++){vec2 fp=s+d*float(i)*0.3;float fd=length(uv-fp);float fs=0.02+float(i)*0.01;fl+=smoothstep(fs*2.0,fs*0.5,fd)*(0.3/float(i));}float p=sin(t)*0.1+0.9;return (mf+fl)*p;}
float turb(vec2 p,float t){float s=1.0;float v=0.0;for(int i=0;i<5;i++){v+=abs(en(p*s+t*0.1*s))/s;s*=2.0;}return v;}
float cloudDens(vec2 uv,float t){
  vec2 drift=vec2(t*0.055,t*0.011);
  float a=en(uv*2.0+drift);float b=en(uv*4.0-drift*1.3);float c=turb(uv*3.0+drift*0.5,t*0.38);
  float pat=a*0.5+b*0.3+c*0.2;return smoothstep(0.42,0.82,pat);
}
float sh(vec2 p){p=fract(p*vec2(234.34,435.345));p+=dot(p,p+34.23);return fract(p.x*p.y);}
vec2 sh2(vec2 p){p=fract(p*vec2(234.34,435.345));p+=dot(p,p+34.23);return fract(vec2(p.x*p.y,p.y*p.x));}
float starsFn(vec2 uv,float density,float t){
  vec2 gUV=uv*density;vec2 gID=floor(gUV);vec2 gP=fract(gUV);float f=0.0;
  for(int y=-1;y<=1;y++){for(int x=-1;x<=1;x++){
    vec2 off=vec2(float(x),float(y));vec2 cID=gID+off;vec2 sp=sh2(cID);
    if(sh(cID+vec2(12.345,67.890))>0.85){
      vec2 toS=(off+sp-gP);float dist=length(toS)*density;float br=sh(cID+vec2(23.456,78.901))*0.6+0.4;
      float tw=sh(cID+vec2(34.567,89.012))*3.0+2.0;float ph=t*tw+sh(cID)*6.28;float tk=pow(sin(ph)*0.5+0.5,3.0);
      float st=0.0;if(dist<1.5){st=br*(0.3+tk*0.7);if(br>0.7){float cg=max(exp(-abs(toS.x)*density*5.0),exp(-abs(toS.y)*density*5.0))*0.3*tk;st+=cg;}}
      f+=st;
    }
  }}
  return f;
}
void main(){
  vec2 uv=v_uv;float aspect=u_item_width/u_item_height;vec2 uvA=vec2(uv.x*aspect,uv.y);
  vec3 col=computeBase(uv);
  if(u_night<0.5){
    float it=u_time*0.08;vec2 sp=vec2(0.85,0.2);vec2 spA=vec2(sp.x*aspect,sp.y);
    float rays=sunRays(uvA,spA,it);float flare=sunCore(uvA,spA,it);vec3 sc=vec3(1.0,0.95,0.7);
    float glow=1.0-smoothstep(0.0,0.95,length(uvA-spA));col=mix(col,mix(col,vec3(0.95,0.96,0.99),0.55),glow*0.5);
    col=sc*rays+col*(1.0-rays*0.4);
    col=sc*flare+col*(1.0-clamp(flare,0.0,1.0)*0.6);
  } else {
    float it=u_time*0.01;
    float s1=starsFn(uvA,40.0,it);float s2=starsFn(uvA+vec2(0.5,0.3),25.0,it*1.3);float s3=starsFn(uvA+vec2(0.25,0.7),15.0,it*0.9);
    vec3 sRGB=vec3(0.85,0.9,1.0)*s1*0.6+vec3(0.95,0.97,1.0)*s2*0.8+vec3(1.0,0.98,0.95)*s3;
    float sA=clamp(s1*0.6+s2*0.8+s3,0.0,1.0);
    col=sRGB*sA+col*(1.0-sA);
  }
  if(u_cloud_amount>0.01){
    float d=cloudDens(uv,u_time);
    vec3 cc=mix(vec3(0.95,0.96,0.98),vec3(0.30,0.33,0.40),u_night);
    col=mix(col,cc,d*u_cloud_amount);
  }
  float m=cornerMask();float a=m*u_bg_color.a;
  gl_FragColor=vec4(col*a,a);
}
)";

  // --- Cloud / Fog effect (Fog = alternative mode) ---
  constexpr char kCloudFragment[] = R"(
float ch(vec2 p){p=fract(p*vec2(234.34,435.345));p+=dot(p,p+34.23);return fract(p.x*p.y);}
float cn(vec2 p){vec2 i=floor(p);vec2 f=fract(p);f=f*f*(3.0-2.0*f);float a=ch(i),b=ch(i+vec2(1.,0.)),c=ch(i+vec2(0.,1.)),d=ch(i+vec2(1.,1.));return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);}
float turb(vec2 p,float t){float s=1.0;float v=0.0;for(int i=0;i<5;i++){v+=abs(cn(p*s+t*0.1*s))/s;s*=2.0;}return v;}
void main(){
  vec2 uv=v_uv;vec3 base=computeBase(uv);
  float ls1,ls2,ls3,dMin,dMax,bo,pa,driftX,evo;
  if(u_alternative>0.5){ls1=1.0;ls2=2.5;ls3=2.0;dMin=0.1;dMax=0.9;bo=0.75;pa=0.05;driftX=0.032;evo=0.06;}
  else{ls1=2.0;ls2=4.0;ls3=6.0;dMin=0.35;dMax=0.75;bo=0.4;pa=0.15;driftX=0.062;evo=0.13;}
  float t=u_time;
  vec2 d1=vec2(t*driftX,t*driftX*0.3);vec2 d2=vec2(-t*driftX*0.7,t*driftX*0.45);
  float a=cn(uv*ls1+d1);float b=cn(uv*ls2+d2);float c=turb(uv*ls3+d1*0.5,t*evo);
  float dens=smoothstep(dMin,dMax,a*0.5+b*0.3+c*0.2);dens*=sin(t*0.15)*pa+(1.0-pa);
  float op=dens*bo*mix(1.0,0.82,u_night);
  vec3 cc=mix(vec3(0.88,0.90,0.93),vec3(0.42,0.45,0.52),u_night);
  vec3 col=cc*op+base*(1.0-op);
  float m=cornerMask();float aa=m*u_bg_color.a;
  gl_FragColor=vec4(col*aa,aa);
}
)";

  // --- Rain effect (intensity = density; long thin scattered streaks) ---
  constexpr char kRainFragment[] = R"(
float h11(float n){return fract(sin(n*78.233)*43758.5453);}
float rainLayer(vec2 uv,float t,float cols,float rows,float fall,float len,float slant,float seed){
  uv.x += uv.y*slant + seed;
  float xc=uv.x*cols; float colId=floor(xc); float fx=fract(xc)-0.5;
  float cr=h11(colId*1.7+seed);
  float y=uv.y*rows - t*fall*(0.55+cr*1.05) + cr*17.0;
  float rowId=floor(y); float fy=fract(y);
  float present=step(0.64, h11(colId*3.3+rowId*1.1+seed));
  float streak=smoothstep(len,0.0,fy)*smoothstep(0.0,0.04,fy);
  float thin=pow(smoothstep(0.5,0.0,abs(fx)),9.0);
  return present*streak*thin;
}
void main(){
  vec2 uv=v_uv;float aspect=u_item_width/u_item_height;vec2 p=vec2(uv.x*aspect,uv.y);float t=u_time;
  float r=rainLayer(p,t,26.0,4.0,2.2,0.60,0.13,0.0)*0.5
         +rainLayer(p,t,38.0,5.0,2.6,0.62,0.10,7.0)*0.42
         +rainLayer(p,t,54.0,6.0,3.0,0.66,0.07,15.0)*0.34;
  r=clamp(r*u_intensity,0.0,1.0);
  vec3 base=computeBase(uv);float rd=mix(1.0,0.82,u_night);
  vec3 col=mix(base,vec3(0.90,0.93,1.0),r*0.4*rd);
  float m=cornerMask();float a=m*u_bg_color.a;
  gl_FragColor=vec4(col*a,a);
}
)";

  // --- Snow effect (intensity = flake count; soft round flakes, independent sway) ---
  constexpr char kSnowFragment[] = R"(
float h21(vec2 p){return fract(sin(dot(p,vec2(41.31,289.17)))*43758.5453);}
float snowLayer(vec2 uv,float aspect,float t,float cells,float fall,float size,float seed){
  vec2 g=vec2(uv.x*aspect*cells,(uv.y - t*fall)*cells);
  vec2 id=floor(g); vec2 f=fract(g)-0.5;
  float present=step(1.0 - 0.20*u_intensity, h21(id+seed));
  float rnd=h21(id+seed+0.5);
  vec2 off=(vec2(h21(id+seed+1.7),h21(id+seed+4.3))-0.5)*0.5;
  off.x += sin(t*(0.5+rnd*1.3)+rnd*6.2831)*0.12;
  float d=length(f-off);
  return smoothstep(size,size*0.25,d)*present;
}
void main(){
  vec2 uv=v_uv;float aspect=u_item_width/u_item_height;float t=u_time;
  float s=snowLayer(uv,aspect,t, 9.0,0.13,0.15,0.0)
         +snowLayer(uv,aspect,t,15.0,0.21,0.12,5.0)
         +snowLayer(uv,aspect,t,22.0,0.32,0.10,11.0);
  s=clamp(s,0.0,1.0);
  vec3 flake=mix(vec3(1.0), vec3(0.72,0.76,0.85), u_night);
  vec3 col=mix(computeBase(uv),flake,s);
  float m=cornerMask();float a=m*u_bg_color.a;
  gl_FragColor=vec4(col*a,a);
}
)";

  // --- Thunderstorm effect (heavy rain + dark sky + lightning flashes) ---
  constexpr char kThunderFragment[] = R"(
float h11(float n){return fract(sin(n*78.233)*43758.5453);}
float rainLayer(vec2 uv,float t,float cols,float rows,float fall,float len,float slant,float seed){
  uv.x += uv.y*slant + seed;
  float xc=uv.x*cols; float colId=floor(xc); float fx=fract(xc)-0.5;
  float cr=h11(colId*1.7+seed);
  float y=uv.y*rows - t*fall*(0.55+cr*1.05) + cr*17.0;
  float rowId=floor(y); float fy=fract(y);
  float present=step(0.48, h11(colId*3.3+rowId*1.1+seed));
  float streak=smoothstep(len,0.0,fy)*smoothstep(0.0,0.04,fy);
  float thin=pow(smoothstep(0.5,0.0,abs(fx)),9.0);
  return present*streak*thin;
}
float lightning(float t){
  float seg=floor(t*0.55);float r=fract(sin(seg*127.13)*43758.545);
  if(r<0.62) return 0.0;
  float local=fract(t*0.55);float f=exp(-local*8.0)*(0.55+0.45*sin(local*55.0+r*10.0));
  return clamp(f,0.0,1.0)*(0.5+r*0.5);
}
void main(){
  vec2 uv=v_uv;float aspect=u_item_width/u_item_height;vec2 p=vec2(uv.x*aspect,uv.y);float t=u_time;
  float r=rainLayer(p,t,30.0,5.0,2.8,0.56,0.14,0.0)*0.5
         +rainLayer(p,t,44.0,6.0,3.3,0.58,0.10,7.0)*0.44
         +rainLayer(p,t,60.0,8.0,3.9,0.60,0.07,15.0)*0.36;
  r=clamp(r*1.15,0.0,1.0);
  vec3 base=computeBase(uv);float rd=mix(1.0,0.85,u_night);
  vec3 col=mix(base,vec3(0.82,0.86,0.98),r*0.5*rd);
  float lf=lightning(u_time);float grad=1.0-smoothstep(0.0,0.9,uv.y);
  col+=vec3(0.75,0.80,1.0)*lf*(0.35+grad*0.5);
  col=min(col,vec3(1.0));
  float m=cornerMask();float a=m*u_bg_color.a;
  gl_FragColor=vec4(col*a,a);
}
)";

} // namespace

void EffectProgram::ensureInitialized() {
  if (m_programs[0].program.isValid()) {
    return;
  }

  initProgram(0, kSkyFragment);
  initProgram(1, kCloudFragment);
  initProgram(2, kRainFragment);
  initProgram(3, kSnowFragment);
  initProgram(4, kThunderFragment);
}

void EffectProgram::destroy() {
  for (auto& pd : m_programs) {
    pd.program.destroy();
  }
}

void EffectProgram::abandon() noexcept {
  for (auto& pd : m_programs) {
    pd.program.abandon();
  }
}

void EffectProgram::initProgram(std::size_t index, const char* fragSource) {
  std::string fullFrag = std::string(kCommonFragment) + fragSource;

  auto& pd = m_programs[index];
  pd.program.create(kVertexShader, fullFrag.c_str());

  auto id = pd.program.id();
  pd.positionLoc = glGetAttribLocation(id, "a_position");
  pd.surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  pd.quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  pd.rectOriginLoc = glGetUniformLocation(id, "u_rect_origin");
  pd.rectSizeLoc = glGetUniformLocation(id, "u_rect_size");
  pd.transformLoc = glGetUniformLocation(id, "u_transform");
  pd.timeLoc = glGetUniformLocation(id, "u_time");
  pd.itemWidthLoc = glGetUniformLocation(id, "u_item_width");
  pd.itemHeightLoc = glGetUniformLocation(id, "u_item_height");
  pd.bgColorLoc = glGetUniformLocation(id, "u_bg_color");
  pd.radiusLoc = glGetUniformLocation(id, "u_radius");
  pd.alternativeLoc = glGetUniformLocation(id, "u_alternative");
  pd.nightLoc = glGetUniformLocation(id, "u_night");
  pd.cloudAmountLoc = glGetUniformLocation(id, "u_cloud_amount");
  pd.intensityLoc = glGetUniformLocation(id, "u_intensity");
  pd.skyTopLoc = glGetUniformLocation(id, "u_sky_top");
  pd.skyBottomLoc = glGetUniformLocation(id, "u_sky_bottom");

  if (pd.positionLoc < 0 || pd.surfaceSizeLoc < 0 || pd.transformLoc < 0) {
    throw std::runtime_error("failed to query effect shader locations");
  }
}

void EffectProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style, const Mat3& transform
) const {
  if (style.type == EffectType::None || width <= 0.0f || height <= 0.0f) {
    return;
  }

  // Map the effect type to its shader program. Fog shares the Cloud program (alternative=1).
  std::size_t idx = 0;
  bool isFog = false;
  switch (style.type) {
  case EffectType::Sky:
    idx = 0;
    break;
  case EffectType::Cloud:
    idx = 1;
    break;
  case EffectType::Fog:
    idx = 1;
    isFog = true;
    break;
  case EffectType::Rain:
    idx = 2;
    break;
  case EffectType::Snow:
    idx = 3;
    break;
  case EffectType::Thunder:
    idx = 4;
    break;
  case EffectType::None:
    return;
  }
  if (idx >= kEffectCount || !m_programs[idx].program.isValid()) {
    return;
  }

  const auto& pd = m_programs[idx];

  static constexpr float kQuad[] = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(pd.program.id());

  glUniform2f(pd.surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(pd.quadSizeLoc, width, height);
  glUniform2f(pd.rectOriginLoc, 0.0f, 0.0f);
  if (pd.rectSizeLoc >= 0) {
    glUniform2f(pd.rectSizeLoc, width, height);
  }
  glUniformMatrix3fv(pd.transformLoc, 1, GL_FALSE, transform.m.data());

  if (pd.timeLoc >= 0) {
    glUniform1f(pd.timeLoc, style.time);
  }
  if (pd.itemWidthLoc >= 0) {
    glUniform1f(pd.itemWidthLoc, width);
  }
  if (pd.itemHeightLoc >= 0) {
    glUniform1f(pd.itemHeightLoc, height);
  }
  if (pd.bgColorLoc >= 0) {
    glUniform4f(pd.bgColorLoc, style.bgColor.r, style.bgColor.g, style.bgColor.b, style.bgColor.a);
  }
  if (pd.radiusLoc >= 0) {
    glUniform1f(pd.radiusLoc, style.radius);
  }
  if (pd.alternativeLoc >= 0) {
    glUniform1f(pd.alternativeLoc, isFog ? 1.0f : 0.0f);
  }
  if (pd.nightLoc >= 0) {
    glUniform1f(pd.nightLoc, style.night ? 1.0f : 0.0f);
  }
  if (pd.cloudAmountLoc >= 0) {
    glUniform1f(pd.cloudAmountLoc, style.cloudAmount);
  }
  if (pd.intensityLoc >= 0) {
    glUniform1f(pd.intensityLoc, style.intensity);
  }
  if (pd.skyTopLoc >= 0) {
    glUniform3f(pd.skyTopLoc, style.skyTop.r, style.skyTop.g, style.skyTop.b);
  }
  if (pd.skyBottomLoc >= 0) {
    glUniform3f(pd.skyBottomLoc, style.skyBottom.r, style.skyBottom.g, style.skyBottom.b);
  }

  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
