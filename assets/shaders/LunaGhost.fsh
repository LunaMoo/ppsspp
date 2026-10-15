//Note  : Recommend to use PPSSPP with chain shaders for full functionality.
// This shader has 2 sets of configs, separate for in-game and video


#ifdef GL_ES
precision mediump float;
precision mediump int;
#endif

uniform sampler2D sampler0;
uniform sampler2D sampler2;
uniform vec4 u_time;
uniform float u_video;

uniform vec4 u_setting;

varying vec2 v_texcoord0;


void main() {
	vec3 color = texture2D(sampler0, v_texcoord0.xy).xyz;
	vec3 prevcolor = texture2D(sampler2, v_texcoord0.xy).rgb;

	if(u_setting.x==2.0 || (u_setting.x==0.0 && u_video==0.0) || (u_setting.x==1.0 && u_video==1.0)) {
		color=mix(color, prevcolor, u_setting.y);
	}

	gl_FragColor.rgb = color;
	gl_FragColor.a = 1.0;
}
