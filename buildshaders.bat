qsb fsquad.vert -o fsquad.vert.qsb
qsb fsquad.frag -o fsquad.frag.qsb
glslangValidator -V -o raygen.spv raygen.rgen
glslangValidator -V -o closesthit.spv closesthit.rchit
glslangValidator -V -o miss.spv miss.rmiss
