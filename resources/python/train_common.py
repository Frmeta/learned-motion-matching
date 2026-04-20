import struct
import os
from pathlib import Path
import numpy as np
import torch


# Single source of truth for LMM latent dimension used by training scripts.
LMM_LATENT_SIZE = 32


def project_root():
    env_root = os.environ.get('MOTION_MATCHING_ROOT', '').strip()
    if env_root:
        return Path(env_root).expanduser().resolve()
    # resources/python/train_common.py -> project root is two parents up
    return Path(__file__).resolve().parents[2]


def repo_path(*parts):
    return str(project_root().joinpath(*parts))


def bin_path(filename):
    env_bin_dir = os.environ.get('MOTION_MATCHING_BIN_DIR', '').strip()
    if env_bin_dir:
        return str(Path(env_bin_dir).expanduser().resolve().joinpath(filename))
    return repo_path('resources', 'bin', filename)


def expected_feature_count_current_runtime():
    # Must mirror matching_feature_count_expected() in controller.cpp.
    return (
        3 +   # Left Foot Position
        3 +   # Right Foot Position
        3 +   # Left Foot Velocity
        3 +   # Right Foot Velocity
        3 +   # Hip Velocity
        1 +   # Head Y Position
        9 +   # Trajectory Positions
        9 +   # Trajectory Directions
        8 +   # Terrain Heights
        1 +   # Idle Flag
        1 +   # Crouch Flag
        1 +   # Jump Flag
        1 +   # Cartwheel Flag
        3 +   # History Left Foot Position (-20)
        3 +   # History Right Foot Position (-20)
        3 +   # History Left Foot Velocity (-20)
        3 +   # History Right Foot Velocity (-20)
        3 +   # History Hip Velocity (-20)
        3 +   # History Trajectory Position (-20)
        3 +   # History Trajectory Direction (-20)
        3 +   # History Trajectory Position (-40)
        3 +   # History Trajectory Direction (-40)
        2     # History Terrain Heights (-15)
    )


def expected_lmm_latent_size():
    # LMM latent size is a fixed runtime contract.
    return LMM_LATENT_SIZE


def expected_lmm_input_size_current_runtime():
    return expected_feature_count_current_runtime() + expected_lmm_latent_size()


def validate_runtime_compatibility(features, future_toe_positions):
    expected_features = expected_feature_count_current_runtime()
    if features.shape[1] != expected_features:
        raise RuntimeError(
            f'Feature layout mismatch: got {features.shape[1]} features, '
            f'expected {expected_features}. Rebuild features.bin using current controller.cpp layout.'
        )

    if future_toe_positions.shape[1] != 12:
        raise RuntimeError(
            f'Future-toe layout mismatch: got {future_toe_positions.shape[1]} floats per frame, '
            f'expected 12 (3 samples x 2 toes x 2 coords). Rebuild database.bin with current schema.'
        )


def validate_latent_compatibility(latent, expected_frames=None):
    expected_latent = expected_lmm_latent_size()
    if latent.shape[1] != expected_latent:
        raise RuntimeError(
            f'Latent layout mismatch: got {latent.shape[1]}, expected {expected_latent}. '
            f'Re-run train_decompressor.py to regenerate latent.bin and decompressor.bin.'
        )

    if expected_frames is not None and latent.shape[0] != expected_frames:
        raise RuntimeError(
            f'Latent frame count mismatch: got {latent.shape[0]} frames, '
            f'expected {expected_frames}. Re-run train_decompressor.py after rebuilding features/database.'
        )

def load_database(filename):

    with open(filename, 'rb') as f:
        
        nframes, nbones = struct.unpack('II', f.read(8))
        bone_positions = np.frombuffer(f.read(nframes*nbones*3*4), dtype=np.float32, count=nframes*nbones*3).reshape([nframes, nbones, 3])
        
        nframes, nbones = struct.unpack('II', f.read(8))
        bone_velocities = np.frombuffer(f.read(nframes*nbones*3*4), dtype=np.float32, count=nframes*nbones*3).reshape([nframes, nbones, 3])
        
        nframes, nbones = struct.unpack('II', f.read(8))
        bone_rotations = np.frombuffer(f.read(nframes*nbones*4*4), dtype=np.float32, count=nframes*nbones*4).reshape([nframes, nbones, 4])
        
        nframes, nbones = struct.unpack('II', f.read(8))
        bone_angular_velocities = np.frombuffer(f.read(nframes*nbones*3*4), dtype=np.float32, count=nframes*nbones*3).reshape([nframes, nbones, 3])
        
        nbones = struct.unpack('I', f.read(4))[0]
        bone_parents = np.frombuffer(f.read(nbones*4), dtype=np.int32, count=nbones).reshape([nbones])
        
        nranges = struct.unpack('I', f.read(4))[0]
        range_starts = np.frombuffer(f.read(nranges*4), dtype=np.int32, count=nranges).reshape([nranges])
        
        nranges = struct.unpack('I', f.read(4))[0]
        range_stops = np.frombuffer(f.read(nranges*4), dtype=np.int32, count=nranges).reshape([nranges])
        
        nframes, ncontacts = struct.unpack('II', f.read(8))
        contact_states = np.frombuffer(f.read(nframes*ncontacts), dtype=np.int8, count=nframes*ncontacts).reshape([nframes, ncontacts])
        
        # Read future toe positions (task-specific output o*)
        nframes, nfuture_toe = struct.unpack('II', f.read(8))
        future_toe_positions = np.frombuffer(f.read(nframes*nfuture_toe*4), dtype=np.float32, count=nframes*nfuture_toe).reshape([nframes, nfuture_toe])
        
    return {
        'bone_positions': bone_positions,
        'bone_rotations': bone_rotations,
        'bone_velocities': bone_velocities,
        'bone_angular_velocities': bone_angular_velocities,
        'bone_parents': bone_parents,
        'range_starts': range_starts,
        'range_stops': range_stops,
        'contact_states': contact_states,
        'future_toe_positions': future_toe_positions,
    }
        

def load_features(filename):

    with open(filename, 'rb') as f:
        
        nframes, nfeatures = struct.unpack('II', f.read(8))
        features = np.frombuffer(f.read(nframes*nfeatures*4), dtype=np.float32, count=nframes*nfeatures).reshape([nframes, nfeatures])
        
        nfeatures = struct.unpack('I', f.read(4))[0]
        features_offset = np.frombuffer(f.read(nfeatures*4), dtype=np.float32, count=nfeatures).reshape([nfeatures])
        
        nfeatures = struct.unpack('I', f.read(4))[0]
        features_scale = np.frombuffer(f.read(nfeatures*4), dtype=np.float32, count=nfeatures).reshape([nfeatures])
        
    return {
        'features': features,
        'features_offset': features_offset,
        'features_scale': features_scale,
    }
    
    
def load_latent(filename):

    with open(filename, 'rb') as f:
        
        nframes, nfeatures = struct.unpack('II', f.read(8))
        latent = np.frombuffer(f.read(nframes*nfeatures*4), dtype=np.float32, count=nframes*nfeatures).reshape([nframes, nfeatures])
        
    return {
        'latent': latent,
    }
    
    
def save_network(filename, layers, mean_in, std_in, mean_out, std_out):
    
    with torch.no_grad():
        
        with open(filename, 'wb') as f:
            f.write(struct.pack('I', mean_in.shape[0]) + mean_in.cpu().numpy().astype(np.float32).ravel().tobytes())
            f.write(struct.pack('I', std_in.shape[0]) + std_in.cpu().numpy().astype(np.float32).ravel().tobytes())
            f.write(struct.pack('I', mean_out.shape[0]) + mean_out.cpu().numpy().astype(np.float32).ravel().tobytes())
            f.write(struct.pack('I', std_out.shape[0]) + std_out.cpu().numpy().astype(np.float32).ravel().tobytes())
            f.write(struct.pack('I', len(layers)))
            for layer in layers:
                f.write(struct.pack('II', *layer.weight.T.shape) + layer.weight.T.cpu().numpy().astype(np.float32).ravel().tobytes())
                f.write(struct.pack('I', *layer.bias.shape) + layer.bias.cpu().numpy().astype(np.float32).ravel().tobytes())
