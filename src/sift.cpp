//
// Created by exclowd on 16/11/21.
//

#include "sift.hpp"

#include <chrono>
#include <iostream>

namespace sift {

#define TIMEIT(f)                                                                 \
    {                                                                             \
        auto start = std::chrono::steady_clock::now();                            \
        f();                                                                      \
        auto end = std::chrono::steady_clock::now();                              \
        std::chrono::duration<double> duration = end - start;                     \
        std::cout << "elapsed time: " << #f << ": " << duration.count() << "s\n"; \
    }

#define G(x, a, b) ((x).at<double>(a, b))
#define EPS ((double)1e-15)
#define EPS2 ((double)1)

/**
 * Constructor for the sift handler class
 * blur and double the image to construct the base image
 */
sift_handler::sift_handler(cv::Mat &&_base) {
    cv::Mat temp, interpolated, blurred_image;
    _base.convertTo(temp, CV_64F);
    _base.release();
    onex = temp.clone();
    // compute the number of octaves
    cv::Size sz = temp.size();
    octaves = (size_t)std::round(std::log2((double)std::min(sz.width, sz.height))) - 1;
    // interpolate and blur base image
    cv::resize(temp, interpolated, sz * 2, 0, 0, cv::INTER_LINEAR);
    double diff = std::max(std::sqrt(pow(SIGMA, 2) - 4 * pow(assumed_blur, 2)), 0.1);
    cv::GaussianBlur(interpolated, blurred_image, cv::Size(0, 0), diff, diff);
    base = blurred_image;
}

/**
 * Destructor for the sift handler class
 * Clear all the images and keypoints
 */
sift_handler::~sift_handler() {
    base.release();
    onex.release();
    for (auto &octave : images) {
        octave.clear();
    }
    images.clear();
    keypoints.clear();
}

/**
 * Function to calculate the keypoints and show the results
 * calls the necessary functions
 */
void sift_handler::exec() {
    TIMEIT(gen_gaussian_images);
    TIMEIT(gen_dog_images);
    TIMEIT(gen_scale_space_extrema);
    TIMEIT(clean_keypoints);
    cv::Mat out, temp;
    onex.convertTo(temp, CV_8U);
    cv::drawKeypoints(temp, keypoints, out, cv::Scalar_<double>::all(-1), cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
    cv::imshow("Display Image", out);
    cv::waitKey(0);
    // for (auto &octave : images) {
    //     for (auto &img : octave) {
    //         cv::imshow("Display Image", img);
    //         cv::waitKey(0);
    //     }
    // }
}

/**
 * helper function
 */
cv::Mat sift_handler::get() const {
    cv::Mat image;
    base.convertTo(image, CV_8U);
    return image;
}

cv::Mat sift_handler::getImg(const cv::Mat &mat) {
    cv::Mat image;
    mat.convertTo(image, CV_8U);
    return image;
}

/**
 * function to generate gaussian image pyramid
 */
void sift_handler::gen_gaussian_images() {
    // first generate all gaussian kernels
    double k = std::pow(2, 1.0 / SCALES);
    std::array<double, IMAGES> kernel{};
    double prev = SIGMA;
    for (int i = 1; i < (int)IMAGES; i++) {
        double now = prev * k;
        kernel[i] = std::sqrt(std::pow(now, 2) - std::pow(prev, 2));
        prev = now;
    }
    // Now do gaussian blurring
    cv::Mat temp = base.clone();
    images.reserve(octaves);
    for (int i = 0; i < octaves; i++) {
        std::vector<cv::Mat> octave_images(IMAGES);
        // the base image for each octave is just interpolated base image
        octave_images[0] = temp;
        for (int j = 1; j < (int)IMAGES; j++) {
            cv::GaussianBlur(octave_images[j - 1], octave_images[j], cv::Size(), kernel[j], kernel[j]);
        }
        size_t baseid = octave_images.size() - 3;
        cv::resize(octave_images[baseid], temp, cv::Size(), 0.5, 0.5, cv::INTER_NEAREST);
        images.push_back(std::move(octave_images));
    }
}

/**
 * subract images to give the difference of gaussian pyramid
 */
void sift_handler::gen_dog_images() {
    // dog would result vector of size IMAGES - 1
    for (auto &octave : images) {
        std::vector<cv::Mat> dog_images(IMAGES - 1);
        for (int j = 1; j < (int)IMAGES; j++) {
            dog_images[j - 1] = octave[j] - octave[j - 1];
        }
        octave.clear();
        octave = std::move(dog_images);
    }
}

/**
 * iterate over the image and check whether pixel is extema
 * if it is then calculate keypoints after localizing extrema
 */
void sift_handler::gen_scale_space_extrema() {
    for (int oct = 0; oct < octaves; oct++) {
        for (int img = 1; img < (int)IMAGES - 2; img++) {
            cv::Size size = images[oct][img].size();
            cv::parallel_for_(cv::Range(BORDER, size.height - BORDER),
                              scale_space_extrema_parallel(images, oct, img, keypoints));
        }
    }
}

/**
 * remove repeating keypoints and scale back to size of original image
 */
void sift_handler::clean_keypoints() {
    // std::cout << keypoints.size() << std::endl;
    std::sort(keypoints.begin(), keypoints.end(), [&](auto kp1, auto kp2) {
        if (kp1.pt.x != kp2.pt.x) return kp1.pt.x < kp2.pt.x;
        if (kp1.pt.y != kp2.pt.y) return kp1.pt.y < kp2.pt.y;
        if (kp1.size != kp2.size) return kp1.size > kp2.size;
        if (kp1.angle != kp2.angle) return kp1.angle < kp2.angle;
        if (kp1.response != kp2.response) return kp1.response > kp2.response;
        if (kp1.octave != kp2.octave) return kp1.octave > kp2.octave;
        if (kp1.class_id != kp2.class_id) return kp1.class_id > kp2.class_id;
        return false;
    });
    auto last = std::unique(keypoints.begin(), keypoints.end(), [&](auto I, auto J) {
        return !(std::abs(I.pt.x - J.pt.x) > EPS2 or std::abs(I.pt.x - J.pt.x) > EPS2 or
                 std::abs(I.size - J.size) > EPS2 or std::abs(I.angle - J.angle) > EPS2);
    });
    keypoints.erase(last, keypoints.end());
    for (auto &kpt : keypoints) {
        kpt.pt *= 0.5;
        kpt.size *= 0.5;
        kpt.octave = (kpt.octave & ~255) | ((kpt.octave - 1) & 255);
    }
    std::cout << keypoints.size() << std::endl;
}

void sift_handler::scale_space_extrema_parallel::operator()(const cv::Range &range) const {
    cv::Size size = images[oct][img].size();
    const int begin = range.start;
    const int end = range.end;

    for (int i = begin; i < end; i++) {
        for (int j = BORDER; j < (int)(size.width - BORDER); j++) {
            std::vector<cv::Mat> pixel_cube = get_pixel_cube(oct, img, i, j);
            if (is_pixel_extremum(pixel_cube)) {
                cv::KeyPoint kpt;
                auto image_index = localize_extrema(oct, img, i, j, kpt);
                if (image_index < 0) {
                    continue;
                }
                get_keypoint_orientations(oct, image_index, kpt);
            }
        }
    }
}

/**
 * helper function to give a 3*3*3 cube at pt i,j. Used in scale space extrema detection
 */
std::vector<cv::Mat> sift_handler::scale_space_extrema_parallel::get_pixel_cube(int oct, int img, size_t i,
                                                                                size_t j) const {
    cv::Mat first_image = images[oct][img - 1];
    cv::Mat second_image = images[oct][img];
    cv::Mat third_image = images[oct][img + 1];
    cv::Rect r(j - 1, i - 1, 3, 3);
    std::vector<cv::Mat> pixel_cube{first_image(r), second_image(r), third_image(r)};
    return pixel_cube;
}

/**
 * calculate whether center pixel is extremum in 3*3*3 pixel cube
 */
bool sift_handler::scale_space_extrema_parallel::is_pixel_extremum(const std::vector<cv::Mat> &pixel_cube) {
    bool is_maximum = true, is_minimum = true;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                is_maximum &= G(pixel_cube[1], 1, 1) >= G(pixel_cube[k], i, j);
                is_minimum &= G(pixel_cube[1], 1, 1) <= G(pixel_cube[k], i, j);
            }
        }
    }
    return (is_minimum | is_maximum);
}

/**
 * calculate the gradient of the pixel cube in x,y,z directions dD/dx
 */
cv::Mat sift_handler::scale_space_extrema_parallel::get_gradient(const std::vector<cv::Mat> &pixel_cube) {
    cv::Mat grad(3, 1, CV_64F);
    G(grad, 0, 0) = 0.5 * (G(pixel_cube[1], 1, 2) - G(pixel_cube[1], 1, 0));
    G(grad, 1, 0) = 0.5 * (G(pixel_cube[1], 2, 1) - G(pixel_cube[1], 0, 1));
    G(grad, 2, 0) = 0.5 * (G(pixel_cube[2], 1, 1) - G(pixel_cube[0], 1, 1));
    return grad;
}

/**
 * calculate the hessina of the pixel cube dD^2/d^2x
 */
cv::Mat sift_handler::scale_space_extrema_parallel::get_hessian(const std::vector<cv::Mat> &pixel_cube) {
    cv::Mat hess(3, 3, CV_64F);
    G(hess, 0, 0) = G(pixel_cube[1], 1, 2) - 2 * G(pixel_cube[1], 1, 1) + G(pixel_cube[1], 1, 0);
    G(hess, 1, 1) = G(pixel_cube[1], 2, 1) - 2 * G(pixel_cube[1], 1, 1) + G(pixel_cube[1], 0, 1);
    G(hess, 2, 2) = G(pixel_cube[2], 1, 1) - 2 * G(pixel_cube[1], 1, 1) + G(pixel_cube[0], 1, 1);

    G(hess, 0, 1) = G(hess, 1, 0) =
        0.25 * (G(pixel_cube[1], 2, 2) - G(pixel_cube[1], 2, 0) - G(pixel_cube[1], 0, 2) + G(pixel_cube[1], 0, 0));
    G(hess, 0, 2) = G(hess, 2, 0) =
        0.25 * (G(pixel_cube[2], 1, 2) - G(pixel_cube[2], 1, 0) - G(pixel_cube[0], 1, 2) + G(pixel_cube[0], 1, 0));
    G(hess, 1, 2) = G(hess, 2, 1) =
        0.25 * (G(pixel_cube[2], 2, 1) - G(pixel_cube[2], 0, 1) - G(pixel_cube[0], 2, 1) + G(pixel_cube[0], 0, 1));
    return hess;
}

int sift_handler::scale_space_extrema_parallel::localize_extrema(int oct, int img, size_t i, size_t j,
                                                                 cv::KeyPoint &kpt) const {
    constexpr int attempts = 5;
    cv::Size sz = images[oct][0].size();
    int attempt;
    std::vector<cv::Mat> pixel_cube;
    cv::Mat grad, hess, res;
    // try to localize the extrema withing these attempts
    for (attempt = 0; attempt < attempts; attempt++) {
        pixel_cube.clear();
        pixel_cube = get_pixel_cube(oct, img, i, j);
        // gradient
        grad = get_gradient(pixel_cube);
        // hessian
        hess = get_hessian(pixel_cube);
        // solve the equation
        bool temp = cv::solve(hess, grad, res, cv::DECOMP_NORMAL);
        if (!temp) {
            return 0;
        }
        res *= -1;
        // std::cout << temp << " " << hess << "*" << res << "=" << grad << std::endl;
        // only way to get convergence
        if (std::abs(G(res, 0, 0)) < 0.5 && std::abs(G(res, 1, 0)) < 0.5 && std::abs(G(res, 2, 0)) < 0.5) {
            break;
        }
        j += (int)std::round(G(res, 0, 0));
        i += (int)std::round(G(res, 1, 0));
        img += (int)std::round(G(res, 2, 0));

        // extremum is outside search zone
        if (i < BORDER || i >= sz.width - BORDER || j < BORDER || j >= sz.height - BORDER || img < 1 ||
            img >= IMAGES - 2) {
            return -1;
        }
        grad.release();
        hess.release();
        res.release();
    }
    // didn't find any convergence point
    if (attempt == 5) {
        return -1;
    }

    double value = G(pixel_cube[1], 1, 1) + 0.5 * grad.dot(res);

    // thresholding using the value given in the paper
    if (std::abs(value) * SCALES >= contrast_threshold) {
        cv::Mat hess2 = hess(cv::Rect(0, 0, 2, 2));
        double hess_trace = cv::trace(hess2)[0];
        double hess_det = cv::determinant(hess2);
        if (hess_det <= EPS) {
            return -1;
        }
        double ratio = (hess_trace * hess_trace) / hess_det;

        // Below code is reponsible for eliminating edge responses using hessian trace and
        // determinant
        if (ratio < THRESHOLD_EIGEN_RATIO) {
            double keypt_octave = oct + (1 << 8) * img + (1 << 16) * std::round((G(res, 2, 0) + 0.5) * 255);
            double keypt_pt_x = (j + G(res, 0, 0)) * (1 << oct);
            double keypt_pt_y = (i + G(res, 1, 0)) * (1 << oct);
            double keypt_size = SIGMA * (std::pow(2, img + G(res, 2, 0)) / (1.0 * SCALES)) * (1 << (oct + 1));
            double keypt_response = std::abs(value);
            kpt = cv::KeyPoint(keypt_pt_x, keypt_pt_y, keypt_size,
                               -1,  // angle
                               keypt_response, keypt_octave);
            return img;
        }
    }
    return -1;
}

/**
 * calculate the orientation of the keypoint
 * A keypoint at specific position might have multiple orientations.
 */
void sift_handler::scale_space_extrema_parallel::get_keypoint_orientations(int oct, int img, cv::KeyPoint &kpt) const {
    cv::Size sz = images[oct][img].size();

    std::vector<double> hist(BINS), smooth(BINS);
    double base_x = round(kpt.pt.x / double(1 << oct));
    double base_y = round(kpt.pt.y / double(1 << oct));
    double base_size = kpt.size / double(1 << (oct + 1));

    double scale = SCALE_FACTOR * base_size;
    double radius = scale * RADIUS_FACTOR;
    double weight_factor = -0.5 / (scale * scale);

    // Creating a histogram of orientations
    for (int i = -radius; i <= radius; i++) {
        if (base_y + i > 0 and base_y + i < sz.height - 1) {
            for (int j = -radius; j <= radius; j++) {
                if (base_x + j > 0 and base_x + j < sz.width - 1) {
                    double dx = G(images[oct][img], base_y + i, base_x + j + 1) -
                                G(images[oct][img], base_y + i, base_x + j - 1);
                    double dy = G(images[oct][img], base_y + i - 1, base_x + j) -
                                G(images[oct][img], base_y + i + 1, base_x + j);
                    double mag = std::sqrt(dx * dx + dy * dy);
                    double orientation = (std::atan(dy / dx) * 180) / PI;
                    size_t index = ((size_t)std::round((orientation * BINS) / 360)) % BINS;
                    hist[index] += std::exp(weight_factor * (i * i + j * j)) * mag;
                }
            }
        }
    }

    auto circ = [&](int i) { return (i + BINS) % BINS; };

    // Smoothing out the histogram
    for (int i = 0; i < (int)BINS; i++) {
        smooth[i] =
            ((6 * hist[i]) + (4 * (hist[circ(i - 1)] + hist[circ(i + 1)])) + (hist[circ(i - 2)] + hist[circ(i + 2)])) /
            16.;
    }

    // select the Gaussian smoothed image
    double max_orientation = *std::max_element(smooth.begin(), smooth.end());
    for (int i = 0; i < (int)BINS; i++) {
        double l = smooth[circ(i - 1)], r = smooth[circ(i + 1)];
        if (smooth[i] > l && smooth[i] > r) {
            double peak = smooth[i];
            if (peak >= PEAK_RATIO * max_orientation) {
                double interpolated_index = std::fmod((i + 0.5 * (l - r) / (l + r - 2 * peak)), (double)BINS);
                double orientation = 360 - (interpolated_index * 360 / BINS);
                if (std::abs(360 - orientation) < EPS) {
                    orientation = 0;
                }
                cv::KeyPoint new_keypoint = cv::KeyPoint(kpt.pt, kpt.size, orientation, kpt.response, kpt.octave);
                keypoints.push_back(new_keypoint);
            }
        }
    }
}

}  // namespace sift