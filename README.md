# Simple Image Viewer

Image viewer intended for cli usage shows images given as argument or in the current directory.  
Because there is no reason to wait two seconds to see an image using the default Windows thing.
Press 'q' or 'ESC' to quit. 'j' and 'k' walk through the images on the selected directory.

## Building
~~~Console
> cl main.c
~~~

### TODOs
- Correctly get metadata from multiframed imgs for rendering
- Add switch to dump some metadata.
- Add option to make window bigger than img like a frame
