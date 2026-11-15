vec4 worldPosition = getModelMatrix() * vec4(transformed, 1.0);
gl_Position = u_ProjectionView * worldPosition;
