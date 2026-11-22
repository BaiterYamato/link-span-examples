#version 120
uniform sampler2D colortex0;
uniform sampler2D colortex13;
uniform sampler2D colortex14;
varying vec2 texcoord;

void main() {
    vec4 normal = texture2D(colortex13, texcoord);
    vec4 specular = texture2D(colortex14, texcoord);
    gl_FragColor = normal.a > 0.5 ? vec4(specular.r, specular.b, normal.b, specular.a)
                                   : texture2D(colortex0, texcoord);
}
