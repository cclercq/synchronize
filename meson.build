project('synchronize', 'cpp', default_options: [ 'cpp_std=c++14' ])

libav = subproject('av-cpp')
libavdep = libav.get_variable('avcpp_dep')

executable('synchronize', [
	    'src/synchronize.cpp'
          ],
	   dependencies : [ libavdep, dependency('threads') ] )
