#include "graphics.h"
#include "model.h"

extern mat<4,4> ModelView, Perspective; // "OpenGL" state matrices

struct FlatShader : IShader {
    const Model &model;
    vec3 uniform_l; // light direction in clip coordinates
    vec3 tri_eye[3];

    FlatShader(const vec3 l, const Model &m) : model(m) {
        uniform_l = normalized((ModelView*vec4{l.x, l.y, l.z, 0.}).xyz()); // transform the light vector to view coordinates
    }

    virtual vec4 vertex(const int face, const int vert) {
        vec3 v = model.vert(face, vert);                          // current vertex in object coordinates
        vec4 gl_Position = ModelView * vec4{v.x, v.y, v.z, 1.};
        tri_eye[vert] = gl_Position.xyz();                        // in eye coordinates
        return Perspective * gl_Position;                         // in clip coordinates
    }

    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const {
        TGAColor gl_FragColor;                                                    // output color of the fragment
        vec3 n = normalized(cross(tri_eye[1]-tri_eye[0], tri_eye[2]-tri_eye[0])); // triangle normal in eye coordinates
        double diff = std::max(0., n * uniform_l);                                // diffuse light intensity
        for (int channel : {0,1,2})
            gl_FragColor[channel] = std::min<int>(30 + 255*diff, 255);            // a bit of ambient light + diffuse light
        return {false, gl_FragColor};                                             // do not discard the pixel
    }
};


void renderFrame(Model model, float cx, float cy, float cz, float lx, float ly, float lz, char* filename) {
    int width  = 480;      // output image size
    int height = 480;
    vec3  light{ lx, ly, lz}; // light source
    vec3    eye{cx, cy, cz}; // camera position
    vec3 center{ 0, 0.5, 0.5}; // camera direction
    vec3     up{ 0, 1, 0}; // camera up vector

    lookat(eye, center, up);                              // build the ModelView   matrix
    perspective(norm(eye-center));                        // build the Perspective matrix
    viewport(width/16, height/16, width*7/8, height*7/8); // build the Viewport    matrix

    TGAImage framebuffer(width, height, TGAImage::RGB, {177, 195, 209, 255});
    std::vector<double> zbuffer(width*height, -1000.);

    FlatShader shader(light, model);
    for (int f=0; f<model.nfaces(); f++) {    // iterate through all triangles
        vec4 clip[3] = { shader.vertex(f, 0), // assemble the primitive
                         shader.vertex(f, 1),
                         shader.vertex(f, 2) };
        rasterize(clip, shader, zbuffer, framebuffer); // rasterize the primitive
    }

    //framebuffer.write_tga_file(filename);
    return;
}

std::string modeldata =
"# cube.obj\n"
"# Import into Blender with Y-forward, Z-up\n"
"#\n"
"# Vertices:                        Faces:\n"
"#      f-------g                          +-------+ \n"
"#     /.      /|                         /.  5   /|  3 back\n"
"#    / .     / |                        / .     / |\n"
"#   e-------h  |                   2   +-------+ 1|\n"
"#   |  b . .|. c      z          right |  . . .|. +\n"
"#   | .     | /       | /y             | . 4   | /\n"
"#   |.      |/        |/               |.      |/\n"
"#   a-------d         +---- x          +-------+\n"
"#                                           6\n"
"#                                        bottom\n"
"\n"
"g cube\n"
"\n"
"# Vertices\n"
"v 0.0 0.0 0.0  # 1 a\n"
"v 0.0 1.0 0.0  # 2 b\n"
"v 1.0 1.0 0.0  # 3 c\n"
"v 1.0 0.0 0.0  # 4 d\n"
"v 0.0 0.0 1.0  # 5 e\n"
"v 0.0 1.0 1.0  # 6 f\n"
"v 1.0 1.0 1.0  # 7 g\n"
"v 1.0 0.0 1.0  # 8 h\n"
"\n"
"# Normal vectors\n"
"# One for each face. Shared by all vertices in that face.\n"
"vn  1.0  0.0  0.0  # 1 cghd\n"
"vn -1.0  0.0  0.0  # 2 aefb\n"
"vn  0.0  1.0  0.0  # 3 gcbf\n"
"vn  0.0 -1.0  0.0  # 4 dhea\n"
"vn  0.0  0.0  1.0  # 5 hgfe\n"
"vn  0.0  0.0 -1.0  # 6 cdab\n"
"\n"
"# Faces v/vt/vn\n"
"#   3-------2\n"
"#   | -     |\n"
"#   |   #   |  Each face = 2 triangles (ccw)\n"
"#   |     - |            = 1-2-3 + 1-3-4\n"
"#   4-------1\n"
"\n"
"# Face 1: cghd = cgh + chd\n"
"f 3/0/1 7/0/1 8/0/1\n"
"f 3/0/1 8/0/1 4/0/1\n"
"\n"
"# Face 2: aefb = aef + afb\n"
"f 1/0/2 5/0/2 6/0/2\n"
"f 1/0/2 6/0/2 2/0/2\n"
"\n"
"# Face 3: gcbf = gcb + gbf\n"
"f 7/0/3 3/0/3 2/0/3\n"
"f 7/0/3 2/0/3 6/0/3\n"
"\n"
"# Face 4: dhea = dhe + dea\n"
"f 4/0/4 8/0/4 5/0/4\n"
"f 4/0/4 5/0/4 1/0/4\n"
"\n"
"# Face 5: hgfe = hgf + hfe\n"
"f 8/0/5 7/0/5 6/0/5\n"
"f 8/0/5 6/0/5 5/0/5\n"
"\n"
"# Face 6: cdab = cda + cab\n"
"f 3/0/6 4/0/6 1/0/6\n"
"f 3/0/6 1/0/6 2/0/6\n"
;

Model model(modeldata);
float cx = -1;
float cy = 3;
float cz = 2;

float lx = 1;
float ly = 1;
float lz = 2;
unsigned int i;
int renderNextFrame() {
    char filename[100];


    //Model model(argv[1]);

    i++;
    sprintf(filename, "framebuffer_%05d.tga", i);
    printf("File %s\n", filename);
    
    cx = sin((float)i / 20) * 20;
    cy = sin((float)i / 15) * 30;
    cz = sin((float)i / 40) * -30;

    lx = cos((float)i / 20) * 20;
    ly = cos((float)i / 15) * 30;
    lz = cos((float)i / 40) * -30;

    renderFrame(model, cx, cy, cz, lx, ly, lz, filename);

    return 0;
}
