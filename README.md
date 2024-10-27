# Simple Image Viewer

Image viewer intended for cli usage, just pass image path as argument, press 'q' or 'ESC' to quit.  
Because there is no reason to wait two seconds to see an image using default Windows thing.

## Building
~~~Console
> cl main.c
~~~

### TODOs
- Render multiframed images (GIF and TIFF) properly. (webp are ok).
- Walk through img files in target dir.
- Add switch to dump some metadata.
