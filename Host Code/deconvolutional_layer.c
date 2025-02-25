#include "deconvolutional_layer.h"
#include "convolutional_layer.h"
#include "batchnorm_layer.h"
#include "utils.h"
#include "im2col.h"
#include "col2im.h"
#include "blas.h"
#include "gemm.h"

#include <stdio.h>
#include <time.h>


static size_t get_workspace_size(layer l){
    return (size_t)l.h*l.w*l.size*l.size*l.n*sizeof(float);
}

void bilinear_init(layer l)
{
    int i,j,f;
    float center = (l.size-1) / 2.;
    for(f = 0; f < l.n; ++f){
        for(j = 0; j < l.size; ++j){
            for(i = 0; i < l.size; ++i){
                float val = (1 - fabs(i - center)) * (1 - fabs(j - center));
                int c = f%l.c;
                int ind = f*l.size*l.size*l.c + c*l.size*l.size + j*l.size + i;
                l.weights[ind] = val;
            }
        }
    }
}


layer make_deconvolutional_layer(int batch, int h, int w, int c, int n, int size, int stride, int padding, int output_padding, ACTIVATION activation, int batch_normalize, int adam)
{
    int i;
    layer l;
    l.type = DECONVOLUTIONAL;

    l.h = h;
    l.w = w;
    l.c = c;
    l.n = n;
    l.batch = batch;
    l.stride = stride;
    l.size = size;

    l.nweights = c*n*size*size;
    l.nbiases = n;

    l.weights = (float *)calloc(c*n*size*size, sizeof(float));
    l.weight_updates = (float *)calloc(c*n*size*size, sizeof(float));

    l.biases = (float *)calloc(n, sizeof(float));
    l.bias_updates = (float *)calloc(n, sizeof(float));
    //float scale = n/(size*size*c);
    //printf("scale: %f\n", scale);
    float scale = .02;
    for(i = 0; i < c*n*size*size; ++i) l.weights[i] = scale*rand_normal();
    //bilinear_init(l);
    for(i = 0; i < n; ++i){
        l.biases[i] = 0;
    }
    l.pad = padding;
    l.output_padding = output_padding;

    l.out_h = (l.h - 1) * l.stride + l.size - 2*l.pad + l.output_padding;
    l.out_w = (l.w - 1) * l.stride + l.size - 2*l.pad + l.output_padding;
    l.out_c = n;
    l.outputs = l.out_w * l.out_h * l.out_c;
    l.inputs = l.w * l.h * l.c;

    scal_cpu(l.nweights, (float)l.out_w*l.out_h/(l.w*l.h), l.weights, 1);

    l.output = (float *)calloc(l.batch*l.outputs, sizeof(float));
    l.delta  = (float *)calloc(l.batch*l.outputs, sizeof(float));

    l.forward_conv = forward_deconvolutional_layer;
    l.backward = backward_deconvolutional_layer;
    l.update = update_deconvolutional_layer;

    l.batch_normalize = batch_normalize;

    if(batch_normalize){
        l.scales = (float *)calloc(n, sizeof(float));
        l.scale_updates = (float *)calloc(n, sizeof(float));
        for(i = 0; i < n; ++i){
            l.scales[i] = 1;
        }

        l.mean = (float *)calloc(n, sizeof(float));
        l.variance = (float *)calloc(n, sizeof(float));

        l.mean_delta = (float *)calloc(n, sizeof(float));
        l.variance_delta = (float *)calloc(n, sizeof(float));

        l.rolling_mean = (float *)calloc(n, sizeof(float));
        l.rolling_variance = (float *)calloc(n, sizeof(float));
        l.x = (float *)calloc(l.batch*l.outputs, sizeof(float));
        l.x_norm = (float *)calloc(l.batch*l.outputs, sizeof(float));
    }
    if(adam){
        l.m = (float *)calloc(c*n*size*size, sizeof(float));
        l.v = (float *)calloc(c*n*size*size, sizeof(float));
        l.bias_m = (float *)calloc(n, sizeof(float));
        l.scale_m = (float *)calloc(n, sizeof(float));
        l.bias_v = (float *)calloc(n, sizeof(float));
        l.scale_v = (float *)calloc(n, sizeof(float));
    }

    l.activation = activation;
    l.workspace_size = get_workspace_size(l);

    fprintf(stderr, "deconv%5d %2d x%2d /%2d  %4d x%4d x%4d   ->  %4d x%4d x%4d\n", n, size, size, stride, w, h, c, l.out_w, l.out_h, l.out_c);

    return l;
}

void denormalize_deconvolutional_layer(layer l)
{
    int i, j;
    for(i = 0; i < l.n; ++i){
        float scale = l.scales[i]/sqrt(l.rolling_variance[i] + .00001);
        for(j = 0; j < l.c*l.size*l.size; ++j){
            l.weights[i*l.c*l.size*l.size + j] *= scale;
        }
        l.biases[i] -= l.rolling_mean[i] * scale;
        l.scales[i] = 1;
        l.rolling_mean[i] = 0;
        l.rolling_variance[i] = 1;
    }
}

void resize_deconvolutional_layer(layer *l, int h, int w)
{
    l->h = h;
    l->w = w;
    l->out_h = (l->h - 1) * l->stride + l->size - 2*l->pad;
    l->out_w = (l->w - 1) * l->stride + l->size - 2*l->pad;

    l->outputs = l->out_h * l->out_w * l->out_c;
    l->inputs = l->w * l->h * l->c;

    l->output = (float *)realloc(l->output, l->batch*l->outputs*sizeof(float));
    l->delta  = (float *)realloc(l->delta,  l->batch*l->outputs*sizeof(float));
    if(l->batch_normalize){
        l->x = (float *)realloc(l->x, l->batch*l->outputs*sizeof(float));
        l->x_norm  = (float *)realloc(l->x_norm, l->batch*l->outputs*sizeof(float));
    }


    l->workspace_size = get_workspace_size(*l);
}


void backward_deconvolutional_layer(layer l, network net)
{
    int i;

    gradient_array(l.output, l.outputs*l.batch, l.activation, l.delta);

    if(l.batch_normalize){
        backward_batchnorm_layer(l, net);
    } else {
        backward_bias(l.bias_updates, l.delta, l.batch, l.n, l.out_w*l.out_h);
    }

    //if(net.delta) memset(net.delta, 0, l.batch*l.h*l.w*l.c*sizeof(float));

    for(i = 0; i < l.batch; ++i){
        int m = l.c;
        int n = l.size*l.size*l.n;
        int k = l.h*l.w;

        float *a = net.input + i*m*k;
        float *b = net.workspace;
        float *c = l.weight_updates;

        im2col_cpu(l.delta + i*l.outputs, l.out_c, l.out_h, l.out_w, 
                l.size, l.stride, l.pad, b);
        gemm_cpu(0,1,m,n,k,1,a,k,b,k,1,c,n);

        if(net.delta){
            int m = l.c;
            int n = l.h*l.w;
            int k = l.size*l.size*l.n;

            float *a = l.weights;
            float *b = net.workspace;
            float *c = net.delta + i*n*m;

            gemm_cpu(0,0,m,n,k,1,a,k,b,n,1,c,n);
        }
    }
}

void update_deconvolutional_layer(layer l, update_args a)
{
    float learning_rate = a.learning_rate*l.learning_rate_scale;
    float momentum = a.momentum;
    float decay = a.decay;
    int batch = a.batch;

    int size = l.size*l.size*l.c*l.n;
    axpy_cpu(l.n, learning_rate/batch, l.bias_updates, 1, l.biases, 1);
    scal_cpu(l.n, momentum, l.bias_updates, 1);

    if(l.scales){
        axpy_cpu(l.n, learning_rate/batch, l.scale_updates, 1, l.scales, 1);
        scal_cpu(l.n, momentum, l.scale_updates, 1);
    }

    axpy_cpu(size, -decay*batch, l.weights, 1, l.weight_updates, 1);
    axpy_cpu(size, learning_rate/batch, l.weight_updates, 1, l.weights, 1);
    scal_cpu(size, momentum, l.weight_updates, 1);
}



