import quat
import bvh
from scipy.interpolate import griddata
import scipy.signal as signal
import scipy.ndimage as ndimage
import struct
import numpy as np

import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D
plt.style.use('ggplot')

""" Basic function for mirroring animation data with this particular skeleton structure """

def animation_mirror(lrot, lpos, names, parents):

    joints_mirror = np.array([(
        names.index('Left'+n[5:]) if n.startswith('Right') else (
        names.index('Right'+n[4:]) if n.startswith('Left') else 
        names.index(n))) for n in names])

    mirror_pos = np.array([-1, 1, 1])
    mirror_rot = np.array([[-1, -1, 1], [1, 1, -1], [1, 1, -1]])

    grot, gpos = quat.fk(lrot, lpos, parents)

    gpos_mirror = mirror_pos * gpos[:,joints_mirror]
    grot_mirror = quat.from_xform(mirror_rot * quat.to_xform(grot[:,joints_mirror]))
    
    return quat.ik(grot_mirror, gpos_mirror, parents)

""" Files to Process """

files = [
    # Tuple gait flags order: (is_crouch, is_idle, is_jump)
    # Semantic feature order used by runtime/database: idle -> crouch -> jump.
    # We just use a small section of this clip for the standing idle
    ('resources/bvh/pushAndStumble1_subject5.bvh', 194,  351, False, True, False), 
    # Running
    ('resources/bvh/run1_subject5.bvh',             90, 7086, False, False, False), 
    # Walking
    ('resources/bvh/walk1_subject5.bvh',            80, 7791, False, False, False), # decrease file size (original: 7791)
    # Terrain
    ('resources/bvh/obstacles1_subject2.bvh',       231, 1240, False, False, False),
    ('resources/bvh/obstacles1_subject2.bvh',       1780, 4972, False, False, False), # split this into 2, cut animation that's holding the stair
    ('resources/bvh/obstacles2_subject5.bvh',       250, 5750, False, False, False),
    # Climb
    ('resources/bvh/obstacles1_subject1.bvh',       1030, 1700, False, False, False),
    ('resources/bvh/obstacles1_subject1.bvh',       1910, 1990, False, False, False),
    ('resources/bvh/obstacles1_subject1.bvh',       2270, 2720, False, False, False),
    ('resources/bvh/obstacles1_subject1.bvh',       3766, 4027, False, False, False),
    ('resources/bvh/obstacles1_subject1.bvh',       4210, 4448, False, False, False),
    # Jump
    ('resources/bvh/jumps1_subject1.bvh',       1490, 1600, False, False, True),
    ('resources/bvh/fight1_subject2.bvh',       4290, 4390, False, False, True),
    ('resources/bvh/fight1_subject2.bvh',       4408, 4600, False, False, True),
    ('resources/bvh/fight1_subject3.bvh',       4600, 4700, False, False, True),
    ('resources/bvh/fight1_subject3.bvh',       4780, 4850, False, False, True),
    ('resources/bvh/fight1_subject3.bvh',       5800, 5910, False, False, True),
    # Crouch
    # ('resources/bvh/obstacles5_subject3.bvh',       350, 1750, True, False, False),
    # Monkey crouch
    ('resources/bvh/ground2_subject2.bvh',       160, 204, True, False, False), #stand to crouch
    ('resources/bvh/ground2_subject2.bvh',       205, 325, True, True, False), #idle
    ('resources/bvh/ground2_subject2.bvh',       325, 2280, True, False, False),
    ('resources/bvh/ground2_subject2.bvh',       2800, 3000, True, False, False),
    ('resources/bvh/ground2_subject3.bvh',       1035, 1600, True, False, False),
    ('resources/bvh/ground1_subject4.bvh',       3700, 4500, True, False, False),

]

""" We will accumulate data in these lists """

bone_positions = []
bone_velocities = []
bone_rotations = []
bone_angular_velocities = []
bone_parents = []
bone_names = []
    
range_starts = []
range_stops = []
range_metadata = []

contact_states = []
crouch_states = []
idle_states = []
jump_states = []

""" Loop Over Files """

for filename, start, stop, is_crouch, is_idle, is_jump in files:
    
    # For each file we process it mirrored and not mirrored
    for mirror in [False, True]:
    
        """ Load Data """
        
        print('Loading "%s" %s...' % (filename, "(Mirrored)" if mirror else ""))
        
        bvh_data = bvh.load(filename)
        bvh_data['positions'] = bvh_data['positions'][start:stop]
        bvh_data['rotations'] = bvh_data['rotations'][start:stop]
        
        positions = bvh_data['positions']
        rotations = quat.unroll(quat.from_euler(np.radians(bvh_data['rotations']), order=bvh_data['order']))

        # Convert from cm to m
        positions *= 0.01
        
        if mirror:
            rotations, positions = animation_mirror(rotations, positions, bvh_data['names'], bvh_data['parents'])
            rotations = quat.unroll(rotations)
        
        """ Supersample """
        
        nframes = positions.shape[0]
        nbones = positions.shape[1]
        
        # Supersample data to 60 fps
        original_times = np.linspace(0, nframes - 1, nframes)
        sample_times = np.linspace(0, nframes - 1, int(0.9 * (nframes * 2 - 1))) # Speed up data by 10%
        
        # This does a cubic interpolation of the data for supersampling and also speeding up by 10%
        positions = griddata(original_times, positions.reshape([nframes, -1]), sample_times, method='cubic').reshape([len(sample_times), nbones, 3])
        rotations = griddata(original_times, rotations.reshape([nframes, -1]), sample_times, method='cubic').reshape([len(sample_times), nbones, 4])
        
        # Need to re-normalize after super-sampling
        rotations = quat.normalize(rotations)
        
        """ Extract Simulation Bone """
        
        # First compute world space positions/rotations
        global_rotations, global_positions = quat.fk(rotations, positions, bvh_data['parents'])
        
        # Specify joints to use for simulation bone 
        sim_position_joint = bvh_data['names'].index("Spine2")
        sim_rotation_joint = bvh_data['names'].index("Hips")
        
        # Position comes from spine joint
        sim_position = np.array([1.0, 1.0, 1.0]) * global_positions[:,sim_position_joint:sim_position_joint+1] # zeroes out Y
        sim_position = signal.savgol_filter(sim_position, 31, 3, axis=0, mode='interp')
        
        # Direction comes from projected hip forward direction
        sim_direction = np.array([1.0, 0.0, 1.0]) * quat.mul_vec(global_rotations[:,sim_rotation_joint:sim_rotation_joint+1], np.array([0.0, 1.0, 0.0]))

        # We need to re-normalize the direction after both projection and smoothing
        sim_direction = sim_direction / np.sqrt(np.sum(np.square(sim_direction), axis=-1))[...,np.newaxis]
        sim_direction = signal.savgol_filter(sim_direction, 61, 3, axis=0, mode='interp')
        sim_direction = sim_direction / np.sqrt(np.sum(np.square(sim_direction), axis=-1)[...,np.newaxis])
        
        # Extract rotation from direction
        sim_rotation = quat.normalize(quat.between(np.array([0, 0, 1]), sim_direction))

        # Transform first joints to be local to sim and append sim as root bone
        positions[:,0:1] = quat.mul_vec(quat.inv(sim_rotation), positions[:,0:1] - sim_position)
        rotations[:,0:1] = quat.mul(quat.inv(sim_rotation), rotations[:,0:1])
        
        positions = np.concatenate([sim_position, positions], axis=1)
        rotations = np.concatenate([sim_rotation, rotations], axis=1)
        
        bone_parents = np.concatenate([[-1], bvh_data['parents'] + 1])
        
        bone_names = ['Simulation'] + bvh_data['names']
        
        """ Compute Velocities """
        
        # Compute velocities via central difference
        velocities = np.empty_like(positions)
        velocities[1:-1] = (
            0.5 * (positions[2:  ] - positions[1:-1]) * 60.0 +
            0.5 * (positions[1:-1] - positions[ :-2]) * 60.0) # [1:-1] cancel each other
        velocities[ 0] = velocities[ 1] - (velocities[ 3] - velocities[ 2]) # linear extrapolation
        velocities[-1] = velocities[-2] + (velocities[-2] - velocities[-3])
        
        # Same for angular velocities
        angular_velocities = np.zeros_like(positions)
        angular_velocities[1:-1] = (
            0.5 * quat.to_scaled_angle_axis(quat.abs(quat.mul_inv(rotations[2:  ], rotations[1:-1]))) * 60.0 +
            0.5 * quat.to_scaled_angle_axis(quat.abs(quat.mul_inv(rotations[1:-1], rotations[ :-2]))) * 60.0)
        angular_velocities[ 0] = angular_velocities[ 1] - (angular_velocities[ 3] - angular_velocities[ 2])
        angular_velocities[-1] = angular_velocities[-2] + (angular_velocities[-2] - angular_velocities[-3])

        """ Compute Contact Data """ 

        global_rotations, global_positions, global_velocities, global_angular_velocities = quat.fk_vel(
            rotations, 
            positions, 
            velocities,
            angular_velocities,
            bone_parents)
        
        contact_velocity_threshold = 0.15
        
        contact_velocity = np.sqrt(np.sum(global_velocities[:,np.array([
            bone_names.index("LeftToe"), 
            bone_names.index("RightToe")])]**2, axis=-1))
        
        # Contacts are given for when contact bones are below velocity threshold
        contacts = contact_velocity < contact_velocity_threshold
        
        # Median filter here acts as a kind of "majority vote", and removes
        # small regions  where contact is either active or inactive
        for ci in range(contacts.shape[1]):
        
            contacts[:,ci] = ndimage.median_filter(
                contacts[:,ci], 
                size=6, 
                mode='nearest')
        
        """ Append to Database """
        
        bone_positions.append(positions)
        bone_velocities.append(velocities)
        bone_rotations.append(rotations)
        bone_angular_velocities.append(angular_velocities)
        
        offset = 0 if len(range_starts) == 0 else range_stops[-1] 

        range_starts.append(offset)
        range_stops.append(offset + len(positions))
        range_metadata.append({
            'range_index': len(range_starts) - 1,
            'db_start': offset,
            'db_stop': offset + len(positions),
            'bvh_name': filename.replace('\\', '/').split('/')[-1],
            'source_start': int(start),
            'source_stop': int(stop),
            'is_mirrored': 1 if mirror else 0,
        })
        
        contact_states.append(contacts)
        crouch_states.append(np.full((len(positions), 1), 1 if is_crouch else 0, dtype=np.uint8))
        idle_states.append(np.full((len(positions), 1), 1 if is_idle else 0, dtype=np.uint8))
        jump_states.append(np.full((len(positions), 1), 1 if is_jump else 0, dtype=np.uint8))
    
    
""" Concatenate Data """
    
bone_positions = np.concatenate(bone_positions, axis=0).astype(np.float32)
bone_velocities = np.concatenate(bone_velocities, axis=0).astype(np.float32)
bone_rotations = np.concatenate(bone_rotations, axis=0).astype(np.float32)
bone_angular_velocities = np.concatenate(bone_angular_velocities, axis=0).astype(np.float32)
bone_parents = bone_parents.astype(np.int32)

range_starts = np.array(range_starts).astype(np.int32)
range_stops = np.array(range_stops).astype(np.int32)

contact_states = np.concatenate(contact_states, axis=0).astype(np.uint8)
crouch_states = np.concatenate(crouch_states, axis=0).astype(np.uint8)
idle_states = np.concatenate(idle_states, axis=0).astype(np.uint8)
jump_states = np.concatenate(jump_states, axis=0).astype(np.uint8)

""" Compute Future Toe Positions for Rough Terrain Navigation """

print("Computing Future Toe Positions...")

# Get total number of frames
nframes = bone_positions.shape[0]

# Get toe joint indices
left_toe_idx = bone_names.index("LeftToe")
right_toe_idx = bone_names.index("RightToe")

# Compute global positions for all frames
global_rotations, global_positions = quat.fk(bone_rotations, bone_positions, bone_parents)

# Initialize future toe positions array (12D per frame: 2 toes × 3 timeframes × 2D)
future_toe_positions = np.zeros([nframes, 12], dtype=np.float32)

# Define look-ahead frames at 60Hz (15, 30, 45 frames = 0.25s, 0.5s, 0.75s)
look_ahead_frames = [15, 30, 45]

# For each frame, compute future toe positions in current frame's local space
for i in range(nframes):
    
    # Find which animation range this frame belongs to
    current_range_idx = -1
    for r in range(len(range_starts)):
        if i >= range_starts[r] and i < range_stops[r]:
            current_range_idx = r
            break
    
    # Get current frame's root rotation and position
    current_root_rot = global_rotations[i, 0]
    current_root_pos = global_positions[i, 0]
    
    # For each look-ahead time
    for t_idx, offset in enumerate(look_ahead_frames):
        future_frame = i + offset
        
        # Clamp to current animation range to handle clip boundaries
        if current_range_idx != -1:
            future_frame = min(future_frame, range_stops[current_range_idx] - 1)
        else:
            future_frame = min(future_frame, nframes - 1)
        
        # Get global toe positions at future frame
        left_toe_global_pos = global_positions[future_frame, left_toe_idx]
        right_toe_global_pos = global_positions[future_frame, right_toe_idx]
        
        # Transform to current frame's local space (character-relative)
        left_toe_local = quat.mul_vec(quat.inv(current_root_rot), left_toe_global_pos - current_root_pos)
        right_toe_local = quat.mul_vec(quat.inv(current_root_rot), right_toe_global_pos - current_root_pos)
        
        # Extract 2D ground-plane coordinates (X and Z, ignoring Y/height)
        # Store in format: [L_x, L_z, R_x, R_z] for each timeframe
        future_toe_positions[i, t_idx * 4 + 0] = left_toe_local[0]   # Left toe X
        future_toe_positions[i, t_idx * 4 + 1] = left_toe_local[2]   # Left toe Z
        future_toe_positions[i, t_idx * 4 + 2] = right_toe_local[0]  # Right toe X
        future_toe_positions[i, t_idx * 4 + 3] = right_toe_local[2]  # Right toe Z

print("Future Toe Positions Computed: %d frames × 12 values" % nframes)

""" Visualize Stats """

if True:

    """ Print Database Summary Stats """

    total_frames = bone_positions.shape[0]
    frame_rate = 60.0  # After supersampling to 60 fps
    total_duration_seconds = total_frames / frame_rate
    total_duration_minutes = total_duration_seconds / 60.0

    print("\n=== Database Summary ===")
    print("Total Frames: %d" % total_frames)
    print("Total Duration: %.2f seconds (%.2f minutes)" % (total_duration_seconds, total_duration_minutes))
    print("Frame Rate: %.1f Hz" % frame_rate)
    print("Total Bones: %d" % bone_positions.shape[1])
    print("Total Clips/Ranges: %d" % len(range_starts))
    print("=======================\n")

""" Visualize Stats """

if True:
    
    print("Visualizing Stats...")
    
    def load_simulation_data(filename):
    
        with open(filename, 'rb') as f:
            
            nframes = struct.unpack('I', f.read(4))[0]
            simulation_positions = np.frombuffer(f.read(nframes*3*4), dtype=np.float32, count=nframes*3).reshape([nframes, 3])
            nframes = struct.unpack('I', f.read(4))[0]
            simulation_velocities = np.frombuffer(f.read(nframes*3*4), dtype=np.float32, count=nframes*3).reshape([nframes, 3])
            nframes = struct.unpack('I', f.read(4))[0]
            simulation_accelerations = np.frombuffer(f.read(nframes*3*4), dtype=np.float32, count=nframes*3).reshape([nframes, 3])
            nframes = struct.unpack('I', f.read(4))[0]
            simulation_rotations = np.frombuffer(f.read(nframes*4*4), dtype=np.float32, count=nframes*4).reshape([nframes, 4])
            nframes = struct.unpack('I', f.read(4))[0]
            simulation_angular_velocities = np.frombuffer(f.read(nframes*3*4), dtype=np.float32, count=nframes*3).reshape([nframes, 3])
        
        simulation_velocities = np.sqrt(np.sum(np.square(simulation_velocities), axis=-1))
        simulation_accelerations = np.sqrt(np.sum(np.square(simulation_accelerations), axis=-1))
        simulation_angular_velocities = np.sqrt(np.sum(np.square(simulation_angular_velocities), axis=-1))
        
        return (simulation_positions, simulation_velocities, simulation_accelerations, simulation_rotations, simulation_angular_velocities)
        
    # Load Simulation Data
    
    (simulation_positions_run,
     simulation_velocities_run,
     simulation_accelerations_run,
     simulation_rotations_run,
     simulation_angular_velocities_run) = load_simulation_data('resources/bin/simulation_run.bin')
    
    (simulation_positions_walk,
     simulation_velocities_walk,
     simulation_accelerations_walk,
     simulation_rotations_walk,
     simulation_angular_velocities_walk) = load_simulation_data('resources/bin/simulation_walk.bin')
    
    # Velocities
    
    run_s, run_e = range_starts[2], range_stops[2]
    walk_s, walk_e = range_starts[4], range_stops[4]
    
    root_velocities_walk = np.sqrt(np.sum(np.square(bone_velocities[walk_s:walk_e,0]), axis=-1))
    root_angular_velocities_walk = np.sqrt(np.sum(np.square(bone_angular_velocities[walk_s:walk_e,0]), axis=-1))
    
    # Compute Walk Acceleration
    root_acceleration_walk = np.zeros_like(bone_velocities[walk_s:walk_e,0])
    root_acceleration_walk[1:-1] = (
        0.5 * (bone_velocities[walk_s+2:walk_e-0,0] - bone_velocities[walk_s+1:walk_e-1,0]) * 60.0 +
        0.5 * (bone_velocities[walk_s+1:walk_e-1,0] - bone_velocities[walk_s+0:walk_e-2,0]) * 60.0)
    root_acceleration_walk[0 ] = root_acceleration_walk[ 1]
    root_acceleration_walk[-1] = root_acceleration_walk[-2]
    
    root_acceleration_walk = np.sqrt(np.sum(np.square(root_acceleration_walk), axis=-1))
    
    root_velocities_run = np.sqrt(np.sum(np.square(bone_velocities[run_s:run_e,0]), axis=-1))
    root_angular_velocities_run = np.sqrt(np.sum(np.square(bone_angular_velocities[run_s:run_e,0]), axis=-1))
    
    # Compute Run Acceleration
    root_acceleration_run = np.zeros_like(bone_velocities[run_s:run_e,0])
    root_acceleration_run[1:-1] = (
        0.5 * (bone_velocities[run_s+2:run_e-0,0] - bone_velocities[run_s+1:run_e-1,0]) * 60.0 +
        0.5 * (bone_velocities[run_s+1:run_e-1,0] - bone_velocities[run_s+0:run_e-2,0]) * 60.0)
    root_acceleration_run[ 0] = root_acceleration_run[ 1]
    root_acceleration_run[-1] = root_acceleration_run[-2]
    
    root_acceleration_run = np.sqrt(np.sum(np.square(root_acceleration_run), axis=-1))
    
    # Plot Histograms
    
    colors = plt.rcParams['axes.prop_cycle'].by_key()['color']
    
    fig, ax = plt.subplots(2, 3, figsize=(6.4, 3.6))
    
    ax[0,0].hist(root_velocities_walk, bins=50, density=True, label='data', color=colors[0])
    ax[0,0].hist(simulation_velocities_walk, bins=50, alpha=0.5, density=True, label='simulation', color=colors[2])
    ax[0,0].set_xlim([0, 6])
    ax[0,0].set_title('Velocities\n\n', fontsize=12)
    ax[0,0].axes.yaxis.set_ticklabels([])
    ax[0,0].set_xlabel('$m/s$')
    
    ax[0,1].hist(root_acceleration_walk, bins=50, density=True, label='data', color=colors[0])
    ax[0,1].hist(simulation_accelerations_walk, bins=50, alpha=0.5, density=True, label='simulation', color=colors[2])
    ax[0,1].set_xlim([0, 15])
    ax[0,1].set_title('Accelerations\n\nWalking', fontsize=12)
    ax[0,1].axes.yaxis.set_ticklabels([])
    ax[0,1].set_xlabel('$m/s^2$')
    
    ax[0,2].hist(root_angular_velocities_walk, bins=50, density=True, label='data', color=colors[0])
    ax[0,2].hist(simulation_angular_velocities_walk, bins=50, alpha=0.5, density=True, label='simulation', color=colors[2])
    ax[0,2].set_xlim([0, 7])
    ax[0,2].set_title('Angular Velocities\n\n', fontsize=12)
    ax[0,2].axes.yaxis.set_ticklabels([])
    ax[0,2].set_xlabel('$rad/s$')
    ax[0,2].legend()
    

    ax[1,0].hist(root_velocities_run, bins=50, density=True, label='data', color=colors[1])
    ax[1,0].hist(simulation_velocities_run, bins=50, alpha=0.5, density=True, label='simulation', color=colors[3])
    ax[1,0].set_xlim([0, 6])
    ax[1,0].axes.yaxis.set_ticklabels([])
    ax[1,0].set_xlabel('$m/s$')
    
    ax[1,1].hist(root_acceleration_run, bins=50, density=True, label='data', color=colors[1])
    ax[1,1].hist(simulation_accelerations_run, bins=50, alpha=0.5, density=True, label='simulation', color=colors[3])
    ax[1,1].set_xlim([0, 15])
    ax[1,1].set_title('Running', fontsize=12)
    ax[1,1].axes.yaxis.set_ticklabels([])
    ax[1,1].set_xlabel('$m/s^2$')
    
    ax[1,2].hist(root_angular_velocities_run, bins=50, density=True, label='data', color=colors[1])
    ax[1,2].hist(simulation_angular_velocities_run, bins=50, alpha=0.5, density=True, label='simulation', color=colors[3])
    ax[1,2].set_xlim([0, 7])
    ax[1,2].axes.yaxis.set_ticklabels([])
    ax[1,2].set_xlabel('$rad/s$')
    ax[1,2].legend()
    
    plt.tight_layout()
    plt.show()
    
    # Compute Trajectories
    
    trajectories_sim_walk = []
    
    for i in range(0, len(simulation_positions_walk) - 60, 15):
        traj = quat.mul_vec(
            quat.inv(simulation_rotations_walk[i:i+1]), simulation_positions_walk[i:i+60:5] - simulation_positions_walk[i:i+1])
        trajectories_sim_walk.append(traj)

    trajectories_sim_run = []
    
    for i in range(0, len(simulation_positions_run) - 60, 15):
        traj = quat.mul_vec(
            quat.inv(simulation_rotations_run[i:i+1]), simulation_positions_run[i:i+60:5] - simulation_positions_run[i:i+1])
        trajectories_sim_run.append(traj)
    
    trajectories_root_walk = []
    
    for i in range(walk_s, walk_e - 60, 30):
        traj = quat.mul_vec(
            quat.inv(bone_rotations[i:i+1,0]), bone_positions[i:i+60:5,0] - bone_positions[i:i+1,0])
        trajectories_root_walk.append(traj)

    trajectories_root_run = []

    for i in range(run_s, run_e - 60, 30):
        traj = quat.mul_vec(
            quat.inv(bone_rotations[i:i+1,0]), bone_positions[i:i+60:5,0] - bone_positions[i:i+1,0])
        trajectories_root_run.append(traj)

    # Plot Trajectories

    fig, ax = plt.subplots(1, 2, figsize=(6.4, 3.6))
    
    for ti, traj in enumerate(trajectories_root_walk):
        ax[0].plot(traj[:,0], traj[:,2], c=colors[0], alpha=0.25, label='data' if ti == 0 else None)
            
    for ti, traj in enumerate(trajectories_sim_walk):
        ax[0].plot(traj[:,0], traj[:,2], c=colors[2], alpha=0.12, label='simulation' if ti == 0 else None)
            
    ax[0].set_title('Walking')
    ax[0].set_ylim([-2.5, 4.5])
    ax[0].set_xlim([-3, 3])
    ax[0].set_aspect('equal')
    ax[0].set_xlabel('m')
    ax[0].set_ylabel('m')
    ax[0].legend()
    
    for ti, traj in enumerate(trajectories_root_run):
        ax[1].plot(traj[:,0], traj[:,2], c=colors[1], alpha=0.25, label='data' if ti == 0 else None)

    for ti, traj in enumerate(trajectories_sim_run):
        ax[1].plot(traj[:,0], traj[:,2], c=colors[3], alpha=0.12, label='simulation' if ti == 0 else None)

    ax[1].set_title('Running')
    ax[1].set_ylim([-2.5, 4.5])
    ax[1].set_xlim([-3, 3])
    ax[1].set_aspect('equal')
    ax[1].set_xlabel('m')
    ax[0].set_ylabel('m')
    ax[1].legend()

    plt.tight_layout()
    plt.show()
    
    
""" Write Database """

print("Writing Database...")

with open('resources/bin/database.bin', 'wb') as f:
    
    nframes = bone_positions.shape[0]
    nbones = bone_positions.shape[1]
    nranges = range_starts.shape[0]
    ncontacts = contact_states.shape[1]
    ncrouch = crouch_states.shape[1]
    nidle = idle_states.shape[1]
    njump = jump_states.shape[1]
    nfuture_toe = future_toe_positions.shape[1]  # Should be 12
    
    f.write(struct.pack('II', nframes, nbones) + bone_positions.ravel().tobytes())
    f.write(struct.pack('II', nframes, nbones) + bone_velocities.ravel().tobytes())
    f.write(struct.pack('II', nframes, nbones) + bone_rotations.ravel().tobytes())
    f.write(struct.pack('II', nframes, nbones) + bone_angular_velocities.ravel().tobytes())
    f.write(struct.pack('I', nbones) + bone_parents.ravel().tobytes())
    
    f.write(struct.pack('I', nranges) + range_starts.ravel().tobytes())
    f.write(struct.pack('I', nranges) + range_stops.ravel().tobytes())
    
    f.write(struct.pack('II', nframes, ncontacts) + contact_states.ravel().tobytes())
    
    # Write future toe positions (task-specific output o*)
    f.write(struct.pack('II', nframes, nfuture_toe) + future_toe_positions.ravel().tobytes())

    # Write crouch state (1 column: 1 for crouch clip frames, 0 otherwise)
    f.write(struct.pack('II', nframes, ncrouch) + crouch_states.ravel().tobytes())

    # Write idle state (1 column: 1 for idle clip frames, 0 otherwise)
    f.write(struct.pack('II', nframes, nidle) + idle_states.ravel().tobytes())

    # Write jump state (1 column: 1 for jump clip frames, 0 otherwise)
    f.write(struct.pack('II', nframes, njump) + jump_states.ravel().tobytes())

print("Database written successfully with future toe positions, crouch labels, idle labels, and jump labels!")


""" Write Database CSV (debug/inspection format) """

print("Writing database.csv...")

def _write_csv_section(csv_file, name, data_2d):
    csv_file.write("# %s (rows=%d, cols=%d)\n" % (name, data_2d.shape[0], data_2d.shape[1]))
    for row in data_2d:
        csv_file.write(",".join(str(v) for v in row))
        csv_file.write("\n")


with open('resources/bin/database.csv', 'w') as csv_f:
    csv_preview_frames = min(nframes, 100)

    # Keep a compact metadata header for easier downstream parsing.
    csv_f.write("# Database Metadata\n")
    csv_f.write("nframes,%d\n" % nframes)
    csv_f.write("nframes_csv,%d\n" % csv_preview_frames)
    csv_f.write("nbones,%d\n" % nbones)
    csv_f.write("nranges,%d\n" % nranges)
    csv_f.write("ncontacts,%d\n" % ncontacts)
    csv_f.write("nfuture_toe,%d\n" % nfuture_toe)
    csv_f.write("ncrouch,%d\n" % ncrouch)
    csv_f.write("nidle,%d\n" % nidle)
    csv_f.write("njump,%d\n" % njump)

    _write_csv_section(csv_f, "bone_positions", bone_positions[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "bone_velocities", bone_velocities[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "bone_rotations", bone_rotations[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "bone_angular_velocities", bone_angular_velocities[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "bone_parents", bone_parents.reshape(1, -1))
    _write_csv_section(csv_f, "range_starts", range_starts.reshape(1, -1))
    _write_csv_section(csv_f, "range_stops", range_stops.reshape(1, -1))
    _write_csv_section(csv_f, "contact_states", contact_states[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "future_toe_positions", future_toe_positions[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "crouch_states", crouch_states[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "idle_states", idle_states[:csv_preview_frames].reshape(csv_preview_frames, -1))
    _write_csv_section(csv_f, "jump_states", jump_states[:csv_preview_frames].reshape(csv_preview_frames, -1))

print("database.csv written successfully!")


""" Write Range Metadata CSV (for runtime GUI labels) """

print("Writing range_metadata.csv...")

with open('resources/bin/range_metadata.csv', 'w') as meta_f:
    meta_f.write("range_index,db_start,db_stop,bvh_name,source_start,source_stop,is_mirrored\n")
    for m in range_metadata:
        meta_f.write(
            "%d,%d,%d,%s,%d,%d,%d\n" % (
                m['range_index'],
                m['db_start'],
                m['db_stop'],
                m['bvh_name'],
                m['source_start'],
                m['source_stop'],
                m['is_mirrored']))

print("range_metadata.csv written successfully!")

    
    