# Producer benchmark: native vs legacy

Measured 2026-11-16 on the Pi 4. No rendering engine on either side.

## Method

- Native: `pixil --dry-run --max-commands 500 --seed 1 --trace /dev/null SCRIPT`
- Legacy: `tools/capture_legacy_stream.py SCRIPT -o /dev/null --max-commands 500 --seed 1` (unmodified `Pixil.py`, consumer replaced by a recorder)
- Harness: `tools/bench_producers.py`. Wall time includes process startup on both sides.
- Startup, measured with a 1-command run: native 0.0097 s, legacy 0.71 s.
- Startup-adjusted ratio = (legacy - 0.71) / (native - 0.0097).
- The legacy stream hit the 500-command cap for KITT_Scanner (checked). The other scripts' legacy command counts were assumed to be 500 and were not individually verified.

## Summary (217 scripts)

| Measure | Geometric mean | Median | 25th pct | 75th pct |
|---|---:|---:|---:|---:|
| Raw wall time (legacy / native) | 76.0x | 108.9x | 54.0x | 138.0x |
| Startup-adjusted, all 217 | 105.0x | 138.9x | 47.0x | 307.2x |
| Startup-adjusted, legacy over 2 s (129 scripts) | 72.8x | 96.4x | 26.9x | 159.5x |

Total wall time: native 27.2 s, legacy 1116.7 s, overall 41.1x (raw).

Failed: pachinko_machine.pix (legacy did not finish within the 600 s timeout; 1711-line script).

## Per-script results

| Script | Native (s) | Legacy (s) | Raw ratio | Startup-adjusted ratio |
|---|---:|---:|---:|---:|
| 3D_asterisk.pix | 0.038 | 6.289 | 165.0x | 196.4x |
| 3D_Grid.pix | 0.022 | 3.472 | 161.4x | 233.8x |
| 3D_perspective_bg.pix | 0.006 | 0.948 | 159.3x | 238.3x |
| 3D_perspective_scroll.pix | 0.007 | 1.111 | 163.3x | 401.3x |
| 3D_perspective_scroll4.pix | 0.007 | 1.136 | 164.8x | 426.4x |
| 3D_perspective_scroll4_down3.pix | 0.007 | 1.144 | 169.0x | 433.6x |
| 3D_Shapes.pix | 0.028 | 4.154 | 149.9x | 191.3x |
| Abstract_Geometry.pix | 0.013 | 2.072 | 160.2x | 421.1x |
| Amiga_Boing_Ball.pix | 0.011 | 2.253 | 211.6x | 1543.4x |
| Analog_Clock.pix | 0.014 | 1.537 | 106.9x | 176.6x |
| Asteroid_Miner.pix | 0.097 | 2.416 | 25.0x | 19.6x |
| Atom_Viewer.pix | 0.010 | 1.310 | 134.1x | 600.5x |
| Aurora_Borealis.pix | 0.126 | 14.206 | 112.9x | 116.2x |
| Aurora_Reactor.pix | 0.064 | 6.966 | 108.2x | 114.4x |
| Bar_Graph_Equalizer.pix | 0.035 | 4.407 | 126.2x | 146.6x |
| Billiard_Break.pix | 0.096 | 2.461 | 25.6x | 20.3x |
| Binary_Star_Dance.pix | 0.155 | 2.180 | 14.1x | 10.1x |
| Blob.pix | 1.434 | 3.997 | 2.8x | 2.3x |
| Boid_Digits.pix | 0.158 | 4.871 | 30.7x | 28.0x |
| Boids_Flocking_Simulation.pix | 0.151 | 6.170 | 41.0x | 38.8x |
| bouncing_ball.pix | 0.009 | 1.290 | 145.6x | 579.9x |
| Bouncing_Pixels.pix | 0.033 | 2.856 | 87.5x | 93.6x |
| Breakout.pix | 0.020 | 3.857 | 195.0x | 312.1x |
| Brians_Brain.pix | 0.101 | 2.314 | 22.8x | 17.5x |
| Bridge_Building.pix | 0.026 | 2.857 | 109.2x | 130.3x |
| bubble_pop.pix | 0.010 | 1.218 | 118.1x | 508.2x |
| Building_Construction.pix | 0.018 | 1.358 | 75.5x | 78.3x |
| Burning_Ship.pix | 0.673 | 56.890 | 84.5x | 84.7x |
| Burnout.pix | 0.012 | 1.030 | 88.3x | 163.3x |
| Cafe_Wall_Illusion.pix | 0.059 | 11.107 | 187.2x | 209.5x |
| Chladni_Patterns.pix | 0.191 | 0.843 | 4.4x | 0.7x |
| Chlandi_Patterns.pix | 0.154 | 15.542 | 100.8x | 102.6x |
| Circles.pix | 0.007 | 1.029 | 156.5x | 319.0x |
| Clock_Wave.pix | 0.027 | 2.782 | 103.8x | 121.2x |
| clockwork_gears.pix | 0.089 | 10.223 | 114.3x | 119.3x |
| Cloth_of_Stars.pix | 0.150 | 4.707 | 31.3x | 28.4x |
| Cloth_simulation.pix | 0.253 | 4.928 | 19.5x | 17.4x |
| Color_Blend.pix | 0.015 | 1.937 | 132.6x | 249.7x |
| Color_Mandala.pix | 0.010 | 1.499 | 144.7x | 789.3x |
| Comet_Buzzer.pix | 0.010 | 1.044 | 104.5x | 334.0x |
| Constellation_Map.pix | 0.021 | 1.341 | 63.2x | 54.9x |
| Coral_Rim.pix | 0.350 | 6.460 | 18.5x | 16.9x |
| Cosmic_Nebula.pix | 0.260 | 43.657 | 167.6x | 171.3x |
| Cosmic_Pulse.pix | 0.008 | 1.276 | 156.1x | 566.2x |
| Cracking.pix | 0.016 | 1.828 | 113.3x | 173.9x |
| Crystal_Growth.pix | 0.016 | 1.902 | 120.0x | 193.9x |
| Dancing_Squares.pix | 0.007 | 1.177 | 166.8x | 466.9x |
| Digital_Rain.pix | 0.009 | 2.706 | 294.1x | 1996.2x |
| DNA_Helix_Ribbon.pix | 0.011 | 1.380 | 126.0x | 534.3x |
| Double_Pendulum.pix | 0.040 | 3.100 | 78.5x | 80.2x |
| Drone_Show.pix | 0.047 | 6.431 | 136.3x | 152.7x |
| Dropped_ball.pix | 0.009 | 1.296 | 139.5x | 585.6x |
| Earthquake_Waves.pix | 0.095 | 7.385 | 77.5x | 78.0x |
| Elastic_Collision.pix | 0.037 | 1.426 | 38.5x | 26.2x |
| Electric_arcs.pix | 0.025 | 3.331 | 135.2x | 175.5x |
| Electric_Web.pix | 0.042 | 3.080 | 73.9x | 74.1x |
| Ember_Moths.pix | 0.145 | 8.436 | 58.3x | 57.2x |
| Embers.pix | 0.027 | 2.626 | 97.3x | 110.8x |
| Epicycles.pix | 0.010 | 1.017 | 104.5x | 307.2x |
| etch-a-sketch.pix | 0.007 | 1.027 | 153.3x | 317.4x |
| Expanding_Circles.pix | 0.014 | 1.865 | 128.8x | 241.8x |
| Expanding_Rings.pix | 0.007 | 1.100 | 147.6x | 389.7x |
| Fifteen_Puzzle.pix | 0.010 | 1.133 | 118.6x | 422.9x |
| fireflies.pix | 0.012 | 1.742 | 140.3x | 379.9x |
| Fireworks.pix | 0.021 | 3.127 | 147.0x | 208.8x |
| fish_tank.pix | 0.018 | 3.206 | 181.3x | 312.6x |
| Flappy_Bird.pix | 0.011 | 1.265 | 120.4x | 554.9x |
| Floating_Geometry.pix | 0.010 | 1.291 | 128.2x | 581.5x |
| Formation_Flyer.pix | 0.019 | 1.866 | 96.2x | 119.2x |
| Fractal_Fire.pix | 0.346 | 7.708 | 22.3x | 20.8x |
| Fractal_Trees.pix | 0.082 | 9.316 | 113.7x | 119.2x |
| Fractured_Mirror.pix | 0.062 | 6.970 | 113.2x | 120.6x |
| Frost.pix | 0.014 | 4.136 | 294.7x | 790.7x |
| Galton_Board.pix | 0.203 | 2.483 | 12.3x | 9.2x |
| game_of_life.pix | 0.047 | 1.446 | 30.5x | 19.5x |
| game_of_life_highres.pix | 0.135 | 1.875 | 13.8x | 9.3x |
| Geometric_Kaleidoscope.pix | 0.006 | 0.912 | 140.5x | 202.3x |
| Gravity_Flip.pix | 2.958 | 7.679 | 2.6x | 2.4x |
| Gravity_Wells.pix | 0.071 | 1.566 | 22.1x | 14.0x |
| Hanging_Lanterns.pix | 0.188 | 4.619 | 24.6x | 21.9x |
| Helix_Drift.pix | 0.034 | 4.617 | 134.2x | 158.1x |
| Hex_Grid_Glow.pix | 0.062 | 5.060 | 81.6x | 83.2x |
| Hourglass_Marbles.pix | 1.114 | 7.697 | 6.9x | 6.3x |
| Hyperspace_Jump.pix | 0.012 | 1.278 | 106.7x | 250.0x |
| Hyperspace_Transition.pix | 0.114 | 9.115 | 79.9x | 80.5x |
| Hypnosis.pix | 0.014 | 1.835 | 132.8x | 272.9x |
| icicle_drop.pix | 0.007 | 0.947 | 135.8x | 237.3x |
| Ink_In_Water.pix | 0.093 | 0.844 | 9.0x | 1.6x |
| Ink_Storm.pix | 0.183 | 7.238 | 39.4x | 37.6x |
| Iron_Filings.pix | 0.324 | 6.792 | 20.9x | 19.3x |
| Jellyfish.pix | 0.015 | 1.424 | 94.2x | 132.0x |
| Julia_Morph.pix | 0.840 | 53.747 | 64.0x | 63.9x |
| Kaleidoscope.pix | 0.009 | 1.119 | 128.3x | 409.0x |
| Kaleidoscope_Tumbler.pix | 0.060 | 27.629 | 460.2x | 534.8x |
| Kinetic_Rainbow.pix | 0.031 | 3.576 | 115.2x | 134.4x |
| KITT_Scanner.pix | 0.036 | 68.961 | 1889.5x | 2547.0x |
| Langtons_Ant_Turmites.pix | 0.015 | 1.553 | 100.8x | 147.9x |
| Laser_Mirror.pix | 0.037 | 6.282 | 170.9x | 205.9x |
| Laser_show.pix | 0.014 | 1.749 | 125.6x | 245.9x |
| Lava_Lamp.pix | 0.609 | 16.828 | 27.6x | 26.9x |
| Lightning.pix | 0.007 | 0.934 | 134.2x | 223.8x |
| Lightning_Roots.pix | 0.348 | 6.973 | 20.0x | 18.5x |
| Liquid_Starfield.pix | 0.033 | 4.389 | 133.9x | 159.5x |
| Lissajous_Curves.pix | 0.007 | 1.046 | 142.9x | 336.5x |
| Lissajous_Snake.pix | 0.033 | 2.649 | 80.2x | 83.1x |
| Magic_Carpets.pix | 0.008 | 1.059 | 137.3x | 349.0x |
| Magnetic_Marbles.pix | 0.172 | 5.593 | 32.5x | 30.1x |
| Magnetic_Pendulum.pix | 0.067 | 1.600 | 23.9x | 15.6x |
| Mandelbrot_Zoom.pix | 0.842 | 14.116 | 16.8x | 16.1x |
| Metaballs_Effect.pix | 0.382 | 29.356 | 76.8x | 76.9x |
| Meteor_Shower.pix | 0.119 | 2.134 | 18.0x | 13.1x |
| Missile_Command.pix | 0.016 | 1.929 | 118.6x | 185.5x |
| Moire_Patterns.pix | 0.027 | 2.788 | 102.7x | 119.0x |
| Molecule_Viewer.pix | 0.019 | 2.605 | 138.0x | 206.4x |
| Morphing_Prism.pix | 0.011 | 1.437 | 130.7x | 561.4x |
| Morphing_Starfield.pix | 0.041 | 2.323 | 57.0x | 51.9x |
| Morse_Code.pix | 0.030 | 2.376 | 79.7x | 82.8x |
| Motion_Blindness.pix | 0.133 | 11.275 | 84.6x | 85.5x |
| Mouse_escape.pix | 0.013 | 1.835 | 143.4x | 363.0x |
| Murmuration_Panic.pix | 0.184 | 2.313 | 12.5x | 9.2x |
| Newtons_Cradle.pix | 0.019 | 2.576 | 137.5x | 206.6x |
| Nuclear_Fission.pix | 0.048 | 5.400 | 111.8x | 121.5x |
| Orbital_Wispfield.pix | 0.031 | 3.901 | 126.2x | 150.5x |
| Orbiting_Ripples.pix | 1.463 | 4.235 | 2.9x | 2.4x |
| Oscilloscope_Display.pix | 0.137 | 15.009 | 109.3x | 112.1x |
| Outrun.pix | 0.077 | 8.804 | 114.8x | 120.8x |
| Paint_Drops.pix | 0.034 | 4.279 | 124.0x | 143.9x |
| Parallax_Meadow.pix | 0.010 | 0.843 | 85.8x | 132.8x |
| Particle_Eruption.pix | 0.050 | 2.603 | 52.0x | 47.0x |
| Particle_fountain.pix | 0.018 | 2.567 | 139.9x | 214.6x |
| Particle_Tumbler.pix | 0.131 | 1.295 | 9.9x | 4.8x |
| Pathfinder.pix | 0.021 | 2.096 | 100.8x | 125.0x |
| pen_art.pix | 0.020 | 2.123 | 103.6x | 130.8x |
| Pendulum_Wave.pix | 0.013 | 1.732 | 136.4x | 340.6x |
| Pendulums_Front.pix | 0.012 | 1.784 | 146.4x | 432.6x |
| Perlin_Noise_Particle_System.pix | 0.097 | 9.385 | 96.3x | 98.8x |
| Photon.pix | 0.010 | 1.275 | 132.7x | 565.4x |
| Pinball_Magnet_Table.pix | 0.057 | 2.240 | 39.0x | 32.1x |
| Plasma_Ball.pix | 0.023 | 2.939 | 126.6x | 165.0x |
| Plasma_Tunnel.pix | 1.098 | 3.554 | 3.2x | 2.6x |
| Polygon_Zoom.pix | 0.008 | 1.178 | 143.1x | 467.7x |
| PolyWobble.pix | 0.019 | 3.588 | 192.5x | 321.9x |
| Pong.pix | 0.010 | 1.208 | 120.2x | 497.9x |
| Prayers.pix | 0.017 | 0.945 | 55.1x | 31.6x |
| Psychedelic_Tunnel.pix | 0.016 | 1.948 | 124.3x | 207.5x |
| Pulse_Matrix.pix | 0.020 | 2.628 | 129.6x | 181.3x |
| Qix.pix | 0.008 | 1.236 | 149.7x | 525.7x |
| Quantum_Particle_Collision.pix | 0.017 | 1.269 | 76.2x | 80.3x |
| Radar_Sweep.pix | 0.044 | 4.022 | 91.3x | 96.4x |
| Radioactive_Decay.pix | 0.048 | 3.962 | 82.1x | 84.4x |
| Rain.pix | 0.023 | 3.177 | 138.2x | 185.8x |
| Rain_Drops.pix | 0.008 | 1.226 | 151.8x | 516.4x |
| rainbow_sweep.pix | 0.028 | 24.892 | 885.2x | 1312.7x |
| Rainbow_wave_collapse.pix | 0.014 | 1.622 | 113.2x | 196.9x |
| Raining_down.pix | 0.019 | 3.620 | 195.7x | 330.6x |
| RD_HighRes.pix | 0.460 | 12.084 | 26.3x | 25.2x |
| Reaction_Diffusion.pix | 0.129 | 4.997 | 38.6x | 35.8x |
| RGB_Color_Cube.pix | 0.088 | 11.588 | 131.6x | 138.9x |
| Ring_Gate.pix | 0.124 | 18.353 | 147.6x | 153.9x |
| Ripple_Chase.pix | 0.138 | 1.789 | 13.0x | 8.4x |
| Rolling_Cylinder.pix | 0.022 | 3.694 | 166.1x | 237.8x |
| Rotating_Cube.pix | 0.021 | 2.944 | 142.4x | 203.5x |
| Rule_Playlist.pix | 0.048 | 5.161 | 107.9x | 116.7x |
| Sand_drop.pix | 0.008 | 1.106 | 141.3x | 395.8x |
| Seaweed_Forest.pix | 0.271 | 5.834 | 21.6x | 19.6x |
| Shape_Lock.pix | 0.085 | 2.258 | 26.4x | 20.4x |
| Sheet_Music.pix | 0.017 | 1.682 | 101.6x | 141.9x |
| Shepherd_Moons.pix | 0.103 | 2.512 | 24.4x | 19.3x |
| Sine_Wave_Color_Flow.pix | 0.009 | 1.055 | 117.1x | 345.0x |
| Slime_Mold.pix | 0.407 | 7.431 | 18.2x | 16.9x |
| Slime_Mold_Highways.pix | 0.984 | 12.296 | 12.5x | 11.9x |
| snake.pix | 0.009 | 1.588 | 175.8x | 877.5x |
| Snowflake.pix | 0.017 | 1.808 | 108.9x | 159.0x |
| Soap_Bubbles.pix | 0.303 | 23.279 | 76.7x | 76.8x |
| Solar_Bloom.pix | 0.026 | 2.495 | 96.5x | 110.6x |
| Solar_system.pix | 0.012 | 2.412 | 194.2x | 625.1x |
| Sort_visualizer.pix | 0.014 | 0.986 | 70.1x | 63.3x |
| Space_Flight.pix | 0.009 | 1.010 | 116.9x | 300.0x |
| Space_Invaders.pix | 0.019 | 5.300 | 277.3x | 487.6x |
| Space_Shooter.pix | 0.010 | 0.849 | 82.0x | 139.4x |
| Spider_Web.pix | 0.010 | 1.292 | 128.8x | 581.6x |
| Spinning_Earth.pix | 0.048 | 6.583 | 138.0x | 154.5x |
| Spiral2.pix | 0.010 | 1.454 | 145.6x | 744.3x |
| Spirograph_Pen.pix | 0.013 | 1.665 | 132.1x | 328.7x |
| Spirograph_sequence.pix | 0.006 | 1.137 | 176.4x | 427.1x |
| Split_Brain.pix | 0.143 | 2.113 | 14.7x | 10.5x |
| Spring_Network.pix | 0.070 | 2.553 | 36.4x | 30.5x |
| sprite_with_grid.pix | 0.008 | 1.034 | 133.3x | 324.3x |
| Squares2.pix | 0.008 | 1.000 | 122.7x | 290.2x |
| Starburst.pix | 0.013 | 2.017 | 151.5x | 361.6x |
| Starfield.pix | 0.024 | 3.382 | 140.5x | 185.9x |
| Starfield_Hyperspace.pix | 0.170 | 21.476 | 126.3x | 129.6x |
| Steampunk_Airship.pix | 0.011 | 1.078 | 100.6x | 361.6x |
| String_Art.pix | 0.030 | 3.119 | 103.0x | 117.0x |
| Sunny_Meadow.pix | 0.009 | 1.006 | 111.3x | 296.0x |
| swarm.pix | 0.077 | 1.631 | 21.1x | 13.7x |
| Swaying_Meadow_Grass.pix | 0.271 | 5.463 | 20.1x | 18.2x |
| Tensegrity_Figure.pix | 0.033 | 2.659 | 80.4x | 83.4x |
| Tetris.pix | 0.058 | 11.439 | 197.5x | 222.5x |
| The_Matrix.pix | 0.037 | 2.002 | 54.0x | 47.2x |
| Tide.pix | 0.355 | 6.507 | 18.3x | 16.8x |
| Tornado.pix | 0.128 | 9.060 | 71.0x | 70.8x |
| Torus_Bob_Fly.pix | 0.022 | 1.646 | 76.2x | 78.7x |
| Tron.pix | 0.050 | 4.564 | 90.5x | 94.7x |
| Twin_Star_Vortex.pix | 0.142 | 3.965 | 28.0x | 24.7x |
| Vector_Balls.pix | 0.022 | 1.501 | 67.9x | 63.7x |
| Voronoi_Pattern.pix | 0.344 | 12.311 | 35.8x | 34.8x |
| Vortex_Rings.pix | 0.163 | 2.158 | 13.2x | 9.4x |
| Vortex_Spiral.pix | 0.014 | 1.695 | 124.2x | 249.2x |
| Watching_Eyes.pix | 0.010 | 0.932 | 89.4x | 222.3x |
| Watching_Eyes_2.pix | 0.013 | 0.962 | 71.6x | 67.6x |
| Wave_grid.pix | 0.023 | 2.718 | 120.8x | 156.8x |
| Wave_Interference.pix | 1.196 | 3.658 | 3.1x | 2.5x |
| Whirlpool.pix | 0.282 | 8.731 | 31.0x | 29.5x |
| Wildfire.pix | 0.380 | 8.043 | 21.1x | 19.8x |
| Wireframe_Polyhedra.pix | 0.025 | 1.780 | 72.6x | 72.2x |
| Wormhole.pix | 0.051 | 7.093 | 138.2x | 153.3x |
